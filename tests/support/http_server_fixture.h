#pragma once

#include <cerrno>
#include <system_error>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#if defined(_WIN32)
#include <windows.h>

#include <cwchar>
#include <memory>
#endif

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/ip/udp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/error.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/middleware.h"

#include "worker_task_fixture.h"

#if defined(_WIN32)
#include <windows.h>

#include <cwchar>
#include <memory>
#endif
#include <algorithm>
#include <initializer_list>
#include <optional>
#include <span>
#include <variant>

#include <asio/ip/address.hpp>
#include <asio/socket_base.hpp>
#include <asio/steady_timer.hpp>
#include <openssl/rand.h>
#include <openssl/x509_vfy.h>

#if defined(RUVIA_ENABLE_HTTP2)
#include "ruvia/http/http2_connection.h"
#endif
#if defined(RUVIA_ENABLE_HTTP3)
#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include "ruvia/http/http3_connection.h"
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <future>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task.h"
#include "ruvia/web/http_client.h"
#ifdef RUVIA_ENABLE_DATABASE
#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db.h"
#endif
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis.h"
#endif
#include "ruvia/core/task.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db.h"
#endif
namespace ruvia::test {

// Selects a free loopback TCP port and keeps other test fixtures off it until
// every server using it has stopped. Never listen here: a listening
// reservation steals real test clients.
//
// POSIX: selection binds without reuse, then the socket joins the port's
// SO_REUSEPORT group so every server worker can bind the same port.
//
// Windows: the server's acceptor binds the port exclusively
// (SO_EXCLUSIVEADDRUSE), which no other bound socket may share. The
// reservation therefore releases the port after selection and keeps only a
// process-independent named lease, which stops other fixtures from selecting
// the port. A process outside the test fixtures may still take it before the
// server binds; that bind then fails loudly.
class tcp_port_reservation final {
public:
    explicit tcp_port_reservation(asio::io_context& io)
        : socket_(io) {
        socket_.open(asio::ip::tcp::v4());
#if defined(_WIN32)
        for (;;) {
            socket_.bind({asio::ip::address_v4::loopback(), 0});
            const auto selected = socket_.local_endpoint();
            wchar_t name[64];
            std::swprintf(name, 64, L"Global\\Ruvia.Test.TcpPort.%u", static_cast<unsigned>(selected.port()));
            ::SetLastError(ERROR_SUCCESS);
            port_lease candidate(::CreateEventW(nullptr, TRUE, FALSE, name));
            const auto error = ::GetLastError();
            if (!candidate) {
                throw std::system_error(static_cast<int>(error), asio::system_category(), "failed to reserve TCP test port lease");
            }
            socket_.close();
            if (error == ERROR_ALREADY_EXISTS) {
                // Another fixture holds this port number between its
                // selection and its server's bind.
                socket_.open(asio::ip::tcp::v4());
                continue;
            }
            lease_ = std::move(candidate);
            endpoint_ = selected;
            break;
        }
#else
        socket_.bind({asio::ip::address_v4::loopback(), 0});
        endpoint_ = socket_.local_endpoint();
        socket_.set_option(asio::socket_base::reuse_address(true));
#if defined(SO_REUSEPORT)
        const int enabled = 1;
        if (::setsockopt(socket_.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
            throw std::system_error(errno, asio::system_category(), "failed to share reserved TCP test port");
        }
#else
        throw std::system_error(asio::error::operation_not_supported, "TCP test reservations require SO_REUSEPORT");
#endif
#endif
    }

    [[nodiscard]] asio::ip::tcp::endpoint endpoint() const {
        return endpoint_;
    }

private:
#if defined(_WIN32)
    struct close_lease final {
        void operator()(void* handle) const noexcept {
            ::CloseHandle(handle);
        }
    };
    using port_lease = std::unique_ptr<void, close_lease>;
    // The named object survives until the last handle closes, including when
    // a process exits without running destructors.
    port_lease lease_;
#endif
    asio::ip::tcp::acceptor socket_;
    asio::ip::tcp::endpoint endpoint_;
};

}  // namespace ruvia::test

namespace ruvia::test {

// Selects a free loopback UDP port and keeps other test fixtures off it until
// every server using it has stopped.
//
// POSIX: selection binds the wildcard address (0.0.0.0) without reuse so no
// other socket holds the port number, then joins 0.0.0.0's SO_REUSEPORT group
// so loopback servers (127.0.0.1) can bind the same port. Keeping the
// reservation on 0.0.0.0 rather than 127.0.0.1 leaves the loopback server's
// SO_REUSEPORT socket group pure (indices 0..N-1 belong only to the server's
// workers for SO_ATTACH_REUSEPORT_CBPF), while Linux's address specificity
// preference delivers every loopback datagram to the 127.0.0.1 server sockets.
//
// Windows: SO_REUSEPORT does not exist and a server binds one UDP socket; a
// second socket bound to the port would split its datagrams. The reservation
// therefore releases the UDP port after selection and keeps only a
// process-independent named lease, which stops other fixtures from selecting
// the port. A process outside the test fixtures may still take it before the
// server binds; that bind then fails loudly instead of sharing traffic.
class udp_port_reservation final {
public:
    explicit udp_port_reservation(asio::io_context& io)
        : socket_(io) {
        socket_.open(asio::ip::udp::v4());
#if defined(_WIN32)
        for (;;) {
            socket_.bind({asio::ip::address_v4::loopback(), 0});
            const auto selected = socket_.local_endpoint();
            wchar_t name[64];
            std::swprintf(name, 64, L"Global\\Ruvia.Test.UdpPort.%u", static_cast<unsigned>(selected.port()));
            ::SetLastError(ERROR_SUCCESS);
            port_lease candidate(::CreateEventW(nullptr, TRUE, FALSE, name));
            const auto error = ::GetLastError();
            if (!candidate) {
                throw std::system_error(static_cast<int>(error), asio::system_category(), "failed to reserve UDP test port lease");
            }
            socket_.close();
            if (error == ERROR_ALREADY_EXISTS) {
                // Another fixture holds this port number between its
                // selection and its server's bind.
                socket_.open(asio::ip::udp::v4());
                continue;
            }
            lease_ = std::move(candidate);
            endpoint_ = selected;
            break;
        }
#else
        socket_.bind({asio::ip::address_v4::any(), 0});
        endpoint_ = {asio::ip::address_v4::loopback(), socket_.local_endpoint().port()};
        socket_.set_option(asio::socket_base::reuse_address(true));
#if defined(SO_REUSEPORT)
        const int enabled = 1;
        if (::setsockopt(socket_.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
            throw std::system_error(errno, asio::system_category(), "failed to share reserved UDP test port");
        }
#else
        throw std::system_error(asio::error::operation_not_supported, "UDP test reservations require SO_REUSEPORT");
#endif
#endif
    }

    [[nodiscard]] asio::ip::udp::endpoint endpoint() const {
        return endpoint_;
    }

private:
#if defined(_WIN32)
    struct close_lease final {
        void operator()(void* handle) const noexcept {
            ::CloseHandle(handle);
        }
    };
    using port_lease = std::unique_ptr<void, close_lease>;
    // The named object survives until the last handle closes, including when
    // a process exits without running destructors.
    port_lease lease_;
#endif
    asio::ip::udp::socket socket_;
    asio::ip::udp::endpoint endpoint_;
};

}  // namespace ruvia::test

namespace ruvia::test {

inline int sign_tls_certificate(X509* certificate, EVP_PKEY* key) {
    const auto context_value = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context_value || EVP_DigestSignInit_ex(context_value.get(), nullptr, "SHA256", nullptr, nullptr,
                              key, nullptr) != 1) {
        return 0;
    }
    return X509_sign_ctx(certificate, context_value.get());
}

inline int write_tls_private_key(BIO* output, const EVP_PKEY* key,
    std::string_view password = {}, bool encrypted = false) {
    if (password.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return 0;
    }
    const auto cipher = std::unique_ptr<EVP_CIPHER, decltype(&EVP_CIPHER_free)>(
        encrypted ? EVP_CIPHER_fetch(nullptr, "AES-256-CBC", nullptr) : nullptr, EVP_CIPHER_free);
    if (encrypted && !cipher) {
        return 0;
    }
    const auto* bytes_value = reinterpret_cast<const unsigned char*>(password.empty() ? "" : password.data());
    return PEM_write_bio_PrivateKey_ex(output, key, cipher.get(), encrypted ? bytes_value : nullptr,
        static_cast<int>(password.size()), nullptr, nullptr, nullptr, nullptr);
}

}  // namespace ruvia::test

namespace ruvia::test {

class tls_identity final {
    scoped_temp_directory directory_{"ruvia-client-tls"};

public:
    // A self-signed identity whose subject CN and only DNS subjectAltName are `name`.
    explicit tls_identity(const char* name)
        : tls_identity(name, std::string("DNS:") + name) {}

    // The subject CN is `common_name`; `subject_alt_name` uses OpenSSL's
    // configuration syntax (`DNS:w*.example.com`), and empty omits the extension.
    tls_identity(const char* common_name, std::string_view subject_alt_name, bool elliptic_curve = false)
        : context_(asio::ssl::context::tls_server) {
        std::filesystem::permissions(directory_.path(), std::filesystem::perms::owner_all);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(elliptic_curve
                                                                    ? EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1")
                                                                    : EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", std::size_t{2048}),
            EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new_ex(nullptr, nullptr), X509_free);
        if (!key || !certificate || X509_set_version(certificate.get(), 2) != 1 ||
            ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
            !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) ||
            !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 86400) ||
            X509_set_pubkey(certificate.get(), key.get()) != 1) {
            throw std::runtime_error("cannot generate TLS test identity");
        }
        const auto subject = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
        if (!subject || X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(common_name), -1, -1, 0) != 1 ||
            X509_set_subject_name(certificate.get(), subject.get()) != 1 ||
            X509_set_issuer_name(certificate.get(), subject.get()) != 1) {
            throw std::runtime_error("cannot name TLS test identity");
        }
        if (!subject_alt_name.empty()) {
            X509V3_CTX extension_context{};
            X509V3_set_ctx(&extension_context, certificate.get(), certificate.get(), nullptr, nullptr, 0);
            std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
                X509V3_EXT_nconf_nid(nullptr, &extension_context, NID_subject_alt_name, std::string(subject_alt_name).c_str()), X509_EXTENSION_free);
            if (!extension || X509_add_ext(certificate.get(), extension.get(), -1) != 1) {
                throw std::runtime_error("cannot name TLS test identity");
            }
        }
        if (ruvia::test::sign_tls_certificate(certificate.get(), key.get()) <= 0 ||
            SSL_CTX_use_certificate(context_.native_handle(), certificate.get()) != 1 ||
            SSL_CTX_use_PrivateKey(context_.native_handle(), key.get()) != 1) {
            throw std::runtime_error("cannot sign TLS test identity");
        }
        ca_file_ = directory_.path() / "ca.pem";
        std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(ca_file_.string().c_str(), "w"), BIO_free);
        if (!output || PEM_write_bio_X509(output.get(), certificate.get()) != 1) {
            throw std::runtime_error("cannot write TLS test certificate");
        }
        private_key_file_ = directory_.path() / "key.pem";
        std::unique_ptr<BIO, decltype(&BIO_free)> private_key_output(BIO_new_file(private_key_file_.string().c_str(), "w"), BIO_free);
        if (!private_key_output || write_tls_private_key(private_key_output.get(), key.get()) != 1) {
            throw std::runtime_error("cannot write TLS test private key");
        }
    }
    [[nodiscard]] const std::filesystem::path& private_key_file() const noexcept {
        return private_key_file_;
    }
    asio::ssl::context context_;
    std::filesystem::path ca_file_;

private:
    std::filesystem::path private_key_file_;
};

}  // namespace ruvia::test

namespace ruvia::test {

// Startup hooks remain caller-owned: application retains them across runs.

class app_run final {
public:
    explicit app_run(application& app)
        : app_(app),
          thread_([this] {
              try {
                  app_.run();
              } catch (...) {
                  std::lock_guard lock(mutex_);
                  failure_ = std::current_exception();
              }
          }) {}

    app_run(const app_run&) = delete;
    app_run& operator=(const app_run&) = delete;

    ~app_run() {
        if (thread_.joinable()) {
            app_.stop();
            thread_.join();
        }
    }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (const auto error = failure()) {
            std::rethrow_exception(error);
        }
    }

    [[nodiscard]] std::exception_ptr failure() const {
        std::lock_guard lock(mutex_);
        return failure_;
    }

    [[nodiscard]] std::thread::id id() const noexcept {
        return thread_.get_id();
    }

private:
    application& app_;
    mutable std::mutex mutex_;
    std::exception_ptr failure_;
    std::thread thread_;
};

}  // namespace ruvia::test

namespace ruvia::test {

template <typename stream_type>
[[nodiscard]] inline std::string read_to_end(stream_type& stream, asio::error_code& terminal_error) {
    std::string result{};
    std::array<char, 4096> bytes;
    terminal_error.clear();
    while (!terminal_error) {
        const auto count = stream.read_some(asio::buffer(bytes), terminal_error);
        result.append(bytes.data(), count);
    }
    return result;
}

template <typename stream_type>
[[nodiscard]] inline std::string read_to_end(stream_type& stream) {
    asio::error_code error;
    auto result = read_to_end(stream, error);
    if (error != asio::error::eof && error != asio::ssl::error::stream_truncated) {
        throw std::system_error(error);
    }
    return result;
}

[[nodiscard]] inline std::string exchange(asio::io_context& io,
    const asio::ip::tcp::endpoint& endpoint, std::string_view request) {
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    asio::write(socket, asio::buffer(request));
    return read_to_end(socket);
}

template <typename predicate_type, typename representation_type, typename period_type>
inline void wait_until(predicate_type predicate,
    std::chrono::duration<representation_type, period_type> timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("test observation timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}  // namespace ruvia::test

namespace ruvia::test {

class http_server_fixture final {
public:
    // The application is shared by every case in the executable, so the first
    // fixture constructed fixes its server_config.
    explicit http_server_fixture(asio::io_context& io,
        server_config config = {.worker_count_ = 1, .connection_scan_interval_ = std::chrono::milliseconds(5)})
        : runtime_(&shared_runtime(io, config)) {}

    [[nodiscard]] const asio::ip::tcp::endpoint& endpoint() const noexcept {
        return runtime_->endpoint();
    }

    // Call after closing or shutting down every client belonging to this case.
    // The application stays running for subsequent cases in the same executable.
    void finish() {
        runtime_->finish_case();
    }

    static void reset_shared() {
        shared_runtime_slot().reset();
    }

private:
    class server_runtime final {
    public:
        server_runtime(asio::io_context& io, const server_config& config)
            : reservation_(io),
              endpoint_(reservation_.endpoint()),
              startup_(std::make_shared<startup_state>()) {
            app().server(config).listen({.address_ = "127.0.0.1", .http_ = endpoint_.port()}).on_start([startup = std::weak_ptr<startup_state>(startup_)] {
                if (auto state = startup.lock()) {
                    state->ready_.set_value();
                }
            });
            auto ready = startup_->ready_.get_future();
            runner_ = std::make_unique<app_run>(app());
            try {
                wait_until([&] {
                    return ready.wait_for(std::chrono::seconds(0)) == std::future_status::ready ||
                           runner_->failure() != nullptr;
                },
                    std::chrono::seconds(5));
                rethrow_failure();
                ready.get();
            } catch (...) {
                runner_.reset();
                throw;
            }
        }

        [[nodiscard]] const asio::ip::tcp::endpoint& endpoint() const noexcept {
            return endpoint_;
        }

        void finish_case() {
            wait_until([&] {
                rethrow_failure();
                return app().stats().active_connections_ == 0;
            },
                std::chrono::seconds(5));
        }

    private:
        struct startup_state final {
            std::promise<void> ready_;
        };

        void rethrow_failure() {
            if (const auto failure = runner_->failure()) {
                std::rethrow_exception(failure);
            }
        }

        tcp_port_reservation reservation_;
        asio::ip::tcp::endpoint endpoint_;
        std::shared_ptr<startup_state> startup_;
        std::unique_ptr<app_run> runner_;
    };

    static std::unique_ptr<server_runtime>& shared_runtime_slot() {
        static std::unique_ptr<server_runtime> runtime;
        return runtime;
    }

    static server_runtime& shared_runtime(asio::io_context& io, const server_config& config) {
        auto& slot = shared_runtime_slot();
        if (!slot) {
            slot = std::make_unique<server_runtime>(io, config);
        }
        return *slot;
    }

    server_runtime* runtime_;
};

inline void reset_app_callbacks(application& app_value) {
    http_server_fixture::reset_shared();
    app_value.document_root(nullptr)
        .rate_limit(nullptr)
        .trusted_proxies({})
        .client(nullptr)
#ifdef RUVIA_ENABLE_DATABASE
        .database(nullptr)
#endif
#ifdef RUVIA_ENABLE_REDIS
        .redis(nullptr)
#endif
        .on_start(nullptr)
        .on_stop(nullptr)
        .on_error(nullptr)
        .on_not_found(nullptr)
        .on_access(nullptr)
        .on_connection_admission(nullptr)
        .on_connection_open(nullptr)
        .on_connection_close(nullptr)
        .on_connection_failure(nullptr)
        .on_tls_client_hello(nullptr)
        .on_http2_input(nullptr)
#if defined(RUVIA_ENABLE_HTTP2)
        .on_http2_frame(nullptr)
        .on_http2_headers(nullptr)
#endif
#if defined(RUVIA_ENABLE_HTTP3)
        .on_http3_frame(nullptr)
        .on_http3_field(nullptr)
#endif
        .on_request_stream(nullptr)
        .on_connection(nullptr)
        .blocking_pool(blocking_pool_options{});
}

}  // namespace ruvia::test

namespace ruvia::test {

template <typename callback_type>
class context_callback final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    explicit context_callback(callback_type* callback) noexcept
        : callback_(callback) {}

    ruvia::task<void> invoke(ruvia::context& context_value, ruvia::next&) {
        co_await (*callback_)(context_value);
    }

private:
    callback_type* callback_;
};

template <typename callback_type>
[[nodiscard]] inline ruvia::dispatch_response with_context(
    const ruvia::test_request& request, callback_type&& callback) {
    ruvia::test_app app;
    app.use<context_callback<std::remove_reference_t<callback_type>>>(&callback);
    return app.request(request);
}

}  // namespace ruvia::test

namespace ruvia::test {

// Stateful, isolated RESP store. All map access and connection teardown run on
// its own event loop. Expiry is not simulated: these cases do not test time.
class redis_session_store_peer final {
public:
    explicit redis_session_store_peer(
        std::map<std::string, std::string> store = std::map<std::string, std::string>())
        : acceptor_(io_, {asio::ip::address_v4::loopback(), 0}),
          work_(asio::make_work_guard(io_)),
          store_(std::move(store)) {
        asio::co_spawn(io_, accept(), [this](std::exception_ptr failure) { fail(failure); });
        thread_ = std::thread([this] { io_.run(); });
    }

    ~redis_session_store_peer() {
        asio::post(io_, [this] {
            closing_ = true;
            std::error_code ignored;
            acceptor_.close(ignored);
            for (const auto& socket : sockets_) {
                socket->close(ignored);
            }
            work_.reset();
        });
        thread_.join();
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

private:
    using socket_type = asio::ip::tcp::socket;

    void fail(std::exception_ptr failure) {
        if (failure) {
            failure_ = failure;
            std::error_code ignored;
            for (const auto& socket : sockets_) {
                socket->close(ignored);
            }
        }
    }

    [[nodiscard]] bool closed(const std::error_code& error) const noexcept {
        return error == asio::error::eof || error == asio::error::connection_reset ||
               error == asio::error::connection_aborted ||
               (closing_ && (error == asio::error::operation_aborted || error == asio::error::bad_descriptor));
    }

    asio::awaitable<void> accept() {
        try {
            for (;;) {
                auto socket = std::make_shared<socket_type>(io_);
                co_await acceptor_.async_accept(*socket, asio::use_awaitable);
                sockets_.push_back(socket);
                asio::co_spawn(io_, serve(socket), [this](std::exception_ptr failure) { fail(failure); });
            }
        } catch (const std::system_error& error) {
            if (!closed(error.code())) {
                throw;
            }
        }
    }

    static asio::awaitable<std::string> line(socket_type& socket, std::string& buffer) {
        const auto size = co_await asio::async_read_until(socket, asio::dynamic_buffer(buffer), "\r\n", asio::use_awaitable);
        auto result = std::string(std::string_view(buffer).substr(0, size - 2));
        buffer.erase(0, size);
        co_return result;
    }

    static asio::awaitable<std::vector<std::string>> command(socket_type& socket, std::string& buffer) {
        const auto header = co_await line(socket, buffer);
        if (header.empty() || header.front() != '*') {
            throw std::runtime_error("expected RESP array");
        }
        const auto count = std::get<0>(ruvia::parse_integer<std::size_t>(std::string_view(header).substr(1)));
        std::vector<std::string> args{};
        for (std::size_t index = 0; index < count; ++index) {
            const auto bulk = co_await line(socket, buffer);
            if (bulk.empty() || bulk.front() != '$') {
                throw std::runtime_error("expected RESP bulk string");
            }
            const auto size = std::get<0>(ruvia::parse_integer<std::size_t>(std::string_view(bulk).substr(1)));
            if (buffer.size() < size + 2) {
                co_await asio::async_read(socket, asio::dynamic_buffer(buffer),
                    asio::transfer_exactly(size + 2 - buffer.size()), asio::use_awaitable);
            }
            args.emplace_back(buffer.data(), size);
            buffer.erase(0, size + 2);
        }
        co_return args;
    }

    std::string execute(const std::vector<std::string>& args) {
        if (failure_) {
            std::rethrow_exception(failure_);
        }
        if (args.size() == 2 && args[0] == "GET") {
            const auto found = store_.find(args[1]);
            return found == store_.end() ? std::string("$-1\r\n") : std::move(std::string("$").append(std::to_string(found->second.size()))) + "\r\n" + found->second + "\r\n";
        }
        if (args.size() >= 3 && args[0] == "SET") {
            // Session creation uses NX; an existing key must remain unchanged.
            return std::string(store_.emplace(args[1], args[2]).second ? "+OK\r\n" : "$-1\r\n");
        }
        if (args.size() == 2 && args[0] == "DEL") {
            return std::move(std::string(":").append(std::to_string(store_.erase(args[1])))) + "\r\n";
        }
        if (args.size() == 7 && args[0] == "EVAL" && args[2] == "2") {
            // Atomic compare-and-replace of an existing identity, not a canned
            // script response: missing sources and occupied targets cannot rotate.
            if (!store_.contains(args[3]) || !store_.emplace(args[4], args[5]).second) {
                return std::string(":0\r\n");
            }
            store_.erase(args[3]);
            return std::string(":1\r\n");
        }
        throw std::runtime_error("unsupported session store command");
    }

    asio::awaitable<void> serve(std::shared_ptr<socket_type> socket) {
        std::string buffer{};
        try {
            for (;;) {
                const auto args = co_await command(*socket, buffer);
                const auto reply = execute(args);
                co_await asio::async_write(*socket, asio::buffer(reply), asio::use_awaitable);
            }
        } catch (const std::system_error& error) {
            if (!closed(error.code())) {
                throw;
            }
        }
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::map<std::string, std::string> store_;
    std::vector<std::shared_ptr<socket_type>> sockets_{};
    std::exception_ptr failure_;
    bool closing_{false};
    std::thread thread_;
};

}  // namespace ruvia::test

#if defined(RUVIA_ENABLE_HTTP2)

namespace ruvia::test {

inline void append_frame(std::string& wire, http2_frame_type type, std::uint8_t flags,
    std::uint32_t stream_id, std::string_view payload) {
    std::array<char, http2_frame_header_bytes> header{};
    if (payload.size() > std::numeric_limits<std::uint32_t>::max() ||
        !encode_http2_frame_header(header, static_cast<std::uint32_t>(payload.size()), type, flags, stream_id)) {
        throw std::logic_error("invalid HTTP/2 test frame");
    }
    wire.append(header.data(), header.size());
    wire.append(payload);
}

struct received_frame final {
    http2_frame_header header_;
    std::string payload_{};
};

template <typename stream_type>
[[nodiscard]] inline received_frame read_frame(stream_type& stream,
    std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt) {
    auto& socket = stream.lowest_layer();
    const auto was_non_blocking = socket.non_blocking();
    struct restore_mode final {
        decltype(socket) socket_;
        bool mode_;
        ~restore_mode() {
            asio::error_code ignored;
            socket_.non_blocking(mode_, ignored);
        }
    } restore{socket, was_non_blocking};
    if (deadline) {
        socket.non_blocking(true);
    }
    const auto read_exact = [&](char* bytes, std::size_t count) {
        if (!deadline) {
            asio::read(stream, asio::buffer(bytes, count));
            return;
        }
        while (count != 0) {
            if (std::chrono::steady_clock::now() >= *deadline) {
                throw std::runtime_error("HTTP/2 test frame timed out");
            }
            asio::error_code error;
            const auto received = stream.read_some(asio::buffer(bytes, count), error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (error) {
                throw std::system_error(error);
            }
            bytes += received;
            count -= received;
        }
    };
    std::array<char, http2_frame_header_bytes> bytes{};
    read_exact(bytes.data(), bytes.size());
    const auto header = parse_http2_frame_header(bytes);
    if (!header) {
        throw std::runtime_error("invalid HTTP/2 response frame");
    }
    received_frame result{*header, std::string(header->length_, '\0')};
    read_exact(result.payload_.data(), result.payload_.size());
    return result;
}

}  // namespace ruvia::test

#endif

#if defined(RUVIA_ENABLE_HTTP3)

namespace ruvia::test {

using http3_test_fields = std::vector<std::pair<std::string, std::string>>;

struct http3_test_resumption final {
    std::shared_ptr<SSL_SESSION> session_;
    std::vector<std::byte> transport_parameters_{};
    http3_peer_settings settings_;

    [[nodiscard]] bool permits_early_data() const noexcept {
        return session_ && SSL_SESSION_get_max_early_data(session_.get()) != 0;
    }
};
struct http3_test_response final {
    // The final status; zero until the final response head arrives.
    int status_{0};
    // Regular fields of the final head, as received (lowercase names).
    http3_test_fields headers_{};
    std::string body_{};
    http3_test_fields trailers_{};
    std::vector<int> interim_statuses_{};
    // A non-H3_NO_ERROR failure reported when the stream closes, including a
    // local protocol failure. Transport actions alone do not close a response.
    std::optional<http3_error_code> reset_error_;
    // STOP_SENDING stops the request writer, not the response reader.
    std::optional<http3_error_code> stop_sending_error_;
    // The server did not process the request (RFC 9114 §4.1.1, §5.2): it was
    // at or above a GOAWAY identifier or rejected before its final head, and
    // may be retried on another connection.
    bool unprocessed_{false};

    // The first field named `name`, compared ASCII case-insensitively.
    [[nodiscard]] std::optional<std::string> header(std::string_view name) const {
        for (const auto& [field_name, value] : headers_) {
            if (std::equal(field_name.begin(), field_name.end(), name.begin(), name.end(), [](char left, char right) {
                    return ascii_lower(left) == ascii_lower(right);
                })) {
                return std::string(value);
            }
        }
        return std::nullopt;
    }

private:
    [[nodiscard]] static char ascii_lower(char value) noexcept {
        return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
    }
};

inline void http3_test_fill_random(std::uint8_t* destination, std::size_t size, const ngtcp2_rand_ctx*) {
    if (RAND_bytes(destination, static_cast<int>(size)) != 1) {
        std::terminate();
    }
}

inline int http3_test_new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token,
    std::size_t size, void*) {
    if (RAND_bytes(cid->data, static_cast<int>(size)) != 1 ||
        RAND_bytes(token->data, NGTCP2_STATELESS_RESET_TOKENLEN) != 1) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    cid->datalen = size;
    return 0;
}

[[nodiscard]] inline ngtcp2_cid http3_test_random_connection_id(std::size_t size) {
    ngtcp2_cid cid{};
    if (size > NGTCP2_MAX_CIDLEN || RAND_bytes(cid.data, static_cast<int>(size)) != 1) {
        throw std::runtime_error("cannot generate a QUIC connection ID");
    }
    cid.datalen = size;
    return cid;
}

// The ngtcp2_crypto_ossl QUIC-TLS adapter callbacks for `role`.
[[nodiscard]] inline http3_quic_crypto_callbacks http3_test_crypto_callbacks(http3_role role) noexcept {
    const bool client = role == http3_role::client;
    return {
        .client_initial_ = client ? ngtcp2_crypto_client_initial_cb : nullptr,
        .recv_client_initial_ = client ? nullptr : ngtcp2_crypto_recv_client_initial_cb,
        .recv_crypto_data_ = ngtcp2_crypto_recv_crypto_data_cb,
        .encrypt_ = ngtcp2_crypto_encrypt_cb,
        .decrypt_ = ngtcp2_crypto_decrypt_cb,
        .hp_mask_ = ngtcp2_crypto_hp_mask_cb,
        .recv_retry_ = client ? ngtcp2_crypto_recv_retry_cb : nullptr,
        .update_key_ = ngtcp2_crypto_update_key_cb,
        .delete_crypto_aead_ctx_ = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
        .delete_crypto_cipher_ctx_ = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
        .get_path_challenge_data_ = ngtcp2_crypto_get_path_challenge_data2_cb,
        .version_negotiation_ = ngtcp2_crypto_version_negotiation_cb,
        .rand_ = http3_test_fill_random,
    };
}

// Byte-wise equality of a socket address with one side of a QUIC path.
[[nodiscard]] inline bool http3_test_same_address(std::span<const std::byte> address, const ngtcp2_addr& expected) noexcept {
    const auto bytes = std::as_bytes(std::span(reinterpret_cast<const char*>(expected.addr), expected.addrlen));
    return std::ranges::equal(address, bytes);
}

// The client TLS side of one QUIC connection: TLS 1.3 only, ALPN "h3" only,
// and the server certificate verified against `ca_file` for `server_name`,
// which is also sent as SNI unless it is an IP literal (RFC 6066 §3 allows
// host names only; a literal is matched against an IP subjectAltName).
class http3_test_client_tls final {
public:
    http3_test_client_tls(const std::filesystem::path& ca_file, std::string_view server_name,
        const http3_test_resumption* resumption = nullptr, bool early_data = false) {
        const std::string server_name_text(server_name);
        context_.reset(SSL_CTX_new(TLS_client_method()));
        if (!context_ || SSL_CTX_set_min_proto_version(context_.get(), TLS1_3_VERSION) != 1 ||
            SSL_CTX_load_verify_locations(context_.get(), ca_file.string().c_str(), nullptr) != 1) {
            throw std::runtime_error("HTTP/3 test client cannot load its trust anchor");
        }
        SSL_CTX_set_verify(context_.get(), SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_session_cache_mode(context_.get(), SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
        SSL_CTX_set_app_data(context_.get(), this);
        SSL_CTX_sess_set_new_cb(context_.get(), capture_ticket);
        tls_.reset(SSL_new(context_.get()));
        if (!tls_ || ngtcp2_crypto_ossl_configure_client_session(tls_.get()) != 0) {
            throw std::runtime_error("HTTP/3 test client cannot create its TLS session");
        }
        SSL_set_app_data(tls_.get(), &reference_);
        SSL_set_connect_state(tls_.get());
        static constexpr std::array<unsigned char, 3> h3_alpn{2, 'h', '3'};
        std::error_code literal_error;
        static_cast<void>(asio::ip::make_address(server_name_text.c_str(), literal_error));
        const bool configured =
            SSL_set_alpn_protos(tls_.get(), h3_alpn.data(), static_cast<unsigned int>(h3_alpn.size())) == 0 &&
            (literal_error ? SSL_set_tlsext_host_name(tls_.get(), server_name_text.c_str()) == 1 &&
                                 SSL_set1_host(tls_.get(), server_name_text.c_str()) == 1
                           : X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(tls_.get()), server_name_text.c_str()) == 1);
        ngtcp2_crypto_ossl_ctx* crypto = nullptr;
        if (!configured || ngtcp2_crypto_ossl_ctx_new(&crypto, tls_.get()) != 0) {
            throw std::runtime_error("HTTP/3 test client cannot configure TLS");
        }
        crypto_.reset(crypto);
        if (resumption != nullptr && resumption->session_) {
            std::unique_ptr<SSL_SESSION, free_session> restored(SSL_SESSION_dup(resumption->session_.get()));
            if (!restored || SSL_set_session(tls_.get(), restored.get()) != 1 ||
                SSL_set_quic_tls_early_data_enabled(tls_.get(),
                    early_data && resumption->permits_early_data() ? 1 : 0) != 1) {
                throw std::runtime_error("HTTP/3 test client cannot restore its TLS ticket");
            }
        }
    }

    ~http3_test_client_tls() {
        SSL_set_app_data(tls_.get(), nullptr);
    }

    http3_test_client_tls(const http3_test_client_tls&) = delete;
    http3_test_client_tls& operator=(const http3_test_client_tls&) = delete;
    http3_test_client_tls(http3_test_client_tls&&) = delete;
    http3_test_client_tls& operator=(http3_test_client_tls&&) = delete;

    // Attaches this session to `connection`, whose address must stay stable
    // and which must be destroyed before this session.
    void bind(http3_quic_connection& connection) noexcept {
        reference_.get_conn = get_connection;
        reference_.user_data = &connection;
        connection.set_tls_native_handle(crypto_.get());
    }

    // Attaches this session to a native connection owned by `owner`, which
    // `get_connection` resolves; for clients that drive ngtcp2 themselves.
    void bind(ngtcp2_crypto_get_conn get_connection_of, void* owner, ngtcp2_conn* connection) noexcept {
        reference_.get_conn = get_connection_of;
        reference_.user_data = owner;
        ngtcp2_conn_set_tls_native_handle(connection, crypto_.get());
    }

    [[nodiscard]] bool h3_selected() const noexcept {
        const unsigned char* protocol = nullptr;
        unsigned int protocol_size = 0;
        SSL_get0_alpn_selected(tls_.get(), &protocol, &protocol_size);
        return std::string_view(reinterpret_cast<const char*>(protocol), protocol_size) == "h3";
    }

    // X509_V_OK unless certificate verification failed.
    [[nodiscard]] long verify_result() const noexcept {
        return SSL_get_verify_result(tls_.get());
    }

    [[nodiscard]] bool has_ticket() const noexcept {
        return ticket_ != nullptr;
    }

    [[nodiscard]] std::shared_ptr<SSL_SESSION> ticket() const {
        auto* copy = ticket_ ? SSL_SESSION_dup(ticket_.get()) : nullptr;
        if (copy == nullptr) {
            throw std::runtime_error("HTTP/3 test client cannot retain its resumption ticket");
        }
        return std::shared_ptr<SSL_SESSION>(copy, SSL_SESSION_free);
    }

    [[nodiscard]] bool resumed() const noexcept {
        return SSL_session_reused(tls_.get()) == 1;
    }

    [[nodiscard]] int early_data_status() const noexcept {
        return SSL_get_early_data_status(tls_.get());
    }

private:
    struct free_context final {
        void operator()(SSL_CTX* context) const noexcept {
            SSL_CTX_free(context);
        }
    };
    struct free_tls final {
        void operator()(SSL* tls) const noexcept {
            SSL_free(tls);
        }
    };
    struct free_crypto final {
        void operator()(ngtcp2_crypto_ossl_ctx* crypto) const noexcept {
            ngtcp2_crypto_ossl_ctx_del(crypto);
        }
    };
    struct free_session final {
        void operator()(SSL_SESSION* session) const noexcept {
            SSL_SESSION_free(session);
        }
    };

    static int capture_ticket(SSL* ssl, SSL_SESSION* session) noexcept {
        auto* self = static_cast<http3_test_client_tls*>(SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl)));
        if (SSL_SESSION_up_ref(session) != 1) {
            return 0;
        }
        self->ticket_.reset(session);
        return 0;
    }

    static ngtcp2_conn* get_connection(ngtcp2_crypto_conn_ref* reference) {
        return static_cast<http3_quic_connection*>(reference->user_data)->native_handle();
    }

    std::unique_ptr<SSL_CTX, free_context> context_;
    std::unique_ptr<SSL, free_tls> tls_;
    std::unique_ptr<ngtcp2_crypto_ossl_ctx, free_crypto> crypto_;
    ngtcp2_crypto_conn_ref reference_{};
    std::unique_ptr<SSL_SESSION, free_session> ticket_;
};

// Blocking HTTP/3 client for loopback tests against any compliant server.
// Construction normally completes the QUIC handshake (TLS 1.3, ALPN "h3",
// verified certificate). With a resumable early-data ticket it returns before
// receiving any packet so start_request() really sends 0-RTT. Every wait is
// bounded by timeout, and QUIC timers run from expiry() while waiting.
// Received DATA returns credit only when taken, providing real backpressure.
class http3_test_client final {
public:
    http3_test_client(asio::ip::udp::endpoint server, const std::filesystem::path& ca_file,
        std::string_view server_name, std::chrono::milliseconds timeout,
        const http3_test_resumption* resumption = nullptr, bool early_data = false, bool datagrams = true)
        : timeout_(timeout),
          server_name_(server_name),
          server_(server),
          socket_(io_),
          tls_(ca_file, server_name_, resumption, early_data),
          input_(64 * 1024) {
        // A connected socket receives only the server's datagrams and reports
        // ICMP port-unreachable, so a missing server fails fast.
        socket_.open(server_.protocol());
        socket_.connect(server_);
        socket_.non_blocking(true);
        local_ = socket_.local_endpoint();
        quic_local_ = local_;
        connection_.emplace(http3_quic_connection::client({
            .crypto_callbacks_ = http3_test_crypto_callbacks(http3_role::client),
            .owner_callbacks_ = {.get_new_connection_id_ = http3_test_new_connection_id,
                .handshake_completed_ = handshake_complete,
                .handshake_confirmed_ = handshake_confirmed},
            .source_connection_id_ = http3_test_random_connection_id(16),
            // RFC 9000 §7.2: at least 8 unpredictable bytes.
            .destination_connection_id_ = http3_test_random_connection_id(18),
            .initial_path_ = path(),
            .initial_timestamp_ = timestamp(),
            .callback_context_ = this,
            .http3_ = {.max_stream_output_bytes_ = max_stream_output_bytes,
                .enable_connect_protocol_ = true,
                .enable_h3_datagram_ = datagrams},
        }));
        tls_.bind(*connection_);

        early_attempted_ = early_data && resumption != nullptr && resumption->permits_early_data();
        if (early_attempted_) {
            if (connection_->start_early_http3(resumption->transport_parameters_, resumption->settings_) != http3_quic_status::accepted) {
                throw std::runtime_error("HTTP/3 test client cannot restore early transport settings");
            }
        } else {
            complete_handshake();
        }
    }

    void complete_handshake() {
        if (!wait_until([this] {
                return connection_->handshake_completed() &&
                       connection_->start_http3() == http3_quic_status::accepted;
            },
                timeout_)) {
            const auto verify = tls_.verify_result();
            throw std::runtime_error((std::string("HTTP/3 test client handshake failed: ") +
                                      (verify != X509_V_OK ? std::string(X509_verify_cert_error_string(verify))
                                                           : describe_wait_failure()))
                    .c_str());
        }
        if (!tls_.h3_selected()) {
            throw std::runtime_error("HTTP/3 test client: server did not select ALPN h3");
        }
    }

    [[nodiscard]] http3_test_resumption capture_resumption() {
        complete_handshake();
        wait_or_throw([this] { return tls_.has_ticket(); }, "HTTP/3 TLS ticket did not arrive");
        std::array<std::byte, 512> parameters{};
        const auto size = connection_->encode_early_transport_parameters(parameters);
        if (!size) {
            throw std::runtime_error("HTTP/3 test client cannot remember QUIC parameters");
        }
        const auto bytes = std::span<const std::byte>(parameters.data(), *size);
        return {tls_.ticket(), {bytes.begin(), bytes.end()}, peer_settings()};
    }

    [[nodiscard]] bool resumed() const noexcept {
        return tls_.resumed();
    }

    [[nodiscard]] bool early_data_attempted() const noexcept {
        return early_attempted_;
    }

    [[nodiscard]] bool early_data_accepted() const noexcept {
        return connection_->handshake_completed() && tls_.early_data_status() == SSL_EARLY_DATA_ACCEPTED;
    }

    [[nodiscard]] bool early_data_rejected() const noexcept {
        return early_rejected_;
    }

    ~http3_test_client() {
        if (!connection_->closed()) {
            try {
                close();
            } catch (...) {
                // The peer may already be gone; closing is best effort here.
            }
        }
    }

    http3_test_client(const http3_test_client&) = delete;
    http3_test_client& operator=(const http3_test_client&) = delete;
    http3_test_client(http3_test_client&&) = delete;
    http3_test_client& operator=(http3_test_client&&) = delete;

    // Sends a validated ordinary request with :scheme https and :authority
    // server_name:port. Nonempty content gets an exact typed Content-Length;
    // headers contain only application fields, not managed framing fields.
    [[nodiscard]] http3_test_response request(std::string_view method, std::string_view path,
        std::initializer_list<std::pair<std::string_view, std::string_view>> headers = {}, std::string_view body = {}) {
        return finish_response(open_request(method, path, headers, body, true));
    }

    // Sends a request like request() and returns its stream without waiting
    // for the response; read it with wait_for_response_head(),
    // read_body_chunk() and finish_response().
    [[nodiscard]] std::int64_t start_request(std::string_view method, std::string_view path,
        std::initializer_list<std::pair<std::string_view, std::string_view>> headers = {}, std::string_view body = {}) {
        return open_request(method, path, headers, body, false);
    }

    // Sends a request head whose content the caller streams afterwards with
    // send_stream_data(); `length` becomes its Content-Length.
    [[nodiscard]] std::int64_t start_streaming_request(std::string_view method, std::string_view path,
        std::optional<http_content_length> length = std::nullopt) {
        const auto stream_id = open_stream(false);
        const auto request_authority = authority();
        const http3_regular_request_head_view head{.method_ = method,
            .authority_ = std::optional<borrowed_text>(request_authority),
            .target_ = path,
            .content_ = http_request_content_plan::streaming(length)};
        if (connection_->http3().submit_request_head(stream_id, head) != http3_submit_status::accepted) {
            throw std::runtime_error("HTTP/3 test client cannot submit the streaming request head");
        }
        flush();
        return stream_id;
    }

    // Keep the server handshake incomplete until the request flight is sent.
    // Merely enabling TLS early data does not prevent pacing from deferring
    // request HEADERS until after the normal receive pump completes the handshake.
    [[nodiscard]] std::int64_t start_early_request(std::string_view method, std::string_view path) {
        if (!early_attempted_ || connection_->handshake_completed()) {
            throw std::logic_error("HTTP/3 early request requires an uncompleted resumed handshake");
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout_;
        flush();
        // A server flight proves its Initial has established the CID mapping,
        // including the Windows acceptor-to-worker handoff. Do not consume it.
        wait_readable(deadline);
        if (socket_.available() == 0) {
            throw std::runtime_error("HTTP/3 server Initial did not arrive");
        }
        const auto stream_id = open_request(method, path, {}, {}, false);
        asio::steady_timer pacing(io_);
        for (;;) {
            http3_stream_output pending;
            const auto result = connection_->http3().peek_output(max_udp_payload_bytes, pending);
            if (result.status_ == http3_transport_status::would_block) {
                return stream_id;
            }
            if (result.status_ != http3_transport_status::accepted || connection_->closed() ||
                std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("HTTP/3 early request flight did not drain");
            }
            auto wake = deadline;
            const auto expiry = connection_->expiry();
            if (expiry < static_cast<ngtcp2_tstamp>((std::numeric_limits<std::int64_t>::max)())) {
                wake = (std::min)(wake, std::chrono::steady_clock::time_point(
                                            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                std::chrono::nanoseconds(static_cast<std::int64_t>(expiry)))));
            }
            pacing.expires_at(wake);
            pacing.wait();
            if (timestamp() >= connection_->expiry()) {
                static_cast<void>(connection_->handle_expiry(timestamp()));
                drain_events();
            }
            flush();
        }
    }

    [[nodiscard]] std::int64_t start_tunnel(const http3_connect_request_head_view& request) {
        return open_tunnel(request);
    }

    [[nodiscard]] std::int64_t start_tunnel(const http3_extended_connect_request_head_view& request) {
        return open_tunnel(request);
    }

    void send_stream_data(std::int64_t stream_id, std::span<const std::byte> bytes, bool finish = false) {
        auto& stream = tracked_stream(stream_id);
        auto& codec = connection_->http3();
        while (!bytes.empty()) {
            const auto submitted = codec.submit_data(stream_id, bytes);
            bytes = bytes.subspan(submitted.consumed_bytes_);
            if (submitted.status_ == http3_data_submit_status::backpressured && submitted.consumed_bytes_ == 0) {
                wait_or_throw([&] {
                    return codec.pending_output_bytes(stream_id) < max_stream_output_bytes || response_complete(stream);
                },
                    "HTTP/3 tunnel write did not progress");
                if (response_complete(stream)) {
                    throw std::runtime_error("HTTP/3 tunnel closed during write");
                }
            } else if (submitted.status_ != http3_data_submit_status::complete &&
                       submitted.status_ != http3_data_submit_status::backpressured) {
                throw std::runtime_error("HTTP/3 tunnel DATA was not accepted");
            }
        }
        if (finish && codec.finish_stream(stream_id) != http3_finish_status::accepted) {
            throw std::runtime_error("HTTP/3 tunnel FIN was not accepted");
        }
        flush();
    }

    void send_datagram(std::int64_t stream_id, std::span<const std::byte> payload) {
        wait_or_throw([&] {
            const auto result = connection_->submit_datagram(stream_id, payload);
            if (result == http3_quic_status::would_block) {
                return false;
            }
            if (result != http3_quic_status::accepted) {
                throw std::runtime_error("HTTP/3 DATAGRAM was not accepted");
            }
            return true;
        },
            "HTTP/3 DATAGRAM output did not progress");
        flush();
    }

    [[nodiscard]] std::string receive_datagram(std::int64_t stream_id) {
        auto& stream = tracked_stream(stream_id);
        wait_or_throw([&] { return !stream.datagrams_.empty() || response_complete(stream); },
            "HTTP/3 DATAGRAM did not arrive");
        if (stream.datagrams_.empty()) {
            throw std::runtime_error("HTTP/3 tunnel closed before DATAGRAM");
        }
        auto payload = std::move(stream.datagrams_.front());
        stream.datagrams_.erase(stream.datagrams_.begin());
        return payload;
    }

    [[nodiscard]] http3_peer_settings peer_settings() {
        wait_or_throw([this] { return connection_->http3().peer_settings().has_value(); },
            "HTTP/3 peer SETTINGS did not arrive");
        return *connection_->http3().peer_settings();
    }

    [[nodiscard]] std::uint16_t local_port() const {
        return local_.port();
    }

    // Model a NAT rebinding: switch the UDP source port underneath the
    // connection, which keeps its connection ID and its own view of the path,
    // without initiating QUIC migration (RFC 9000 §9.3).
    void rebind() {
        auto replacement = replacement_socket();
        local_ = replacement.local_endpoint();
        socket_ = std::move(replacement);
    }

    void migrate() {
        complete_handshake();
        wait_or_throw([this] { return handshake_confirmed_; },
            "HTTP/3 handshake confirmation did not arrive");
        migration_socket_.emplace(replacement_socket());
        migration_local_ = migration_socket_->local_endpoint();
        const auto candidate = path(migration_local_);
        http3_quic_result result;
        // Consecutive migrations can consume the spare CIDs before the peer's
        // replacement NEW_CONNECTION_ID frames arrive. Drive that protocol
        // backpressure, rather than treating would_block as a failed migration.
        wait_or_throw([&] {
            result = connection_->migrate(candidate, timestamp());
            return result.status_ != http3_quic_status::would_block;
        },
            "HTTP/3 client migration connection ID did not arrive");
        if (result.status_ != http3_quic_status::accepted) {
            migration_socket_.reset();
            throw std::runtime_error("HTTP/3 client migration was not accepted");
        }
        wait_or_throw([&] {
            const auto current = connection_->current_path();
            return http3_test_same_address(current.local_, candidate.local) &&
                   http3_test_same_address(current.remote_, candidate.remote);
        },
            "HTTP/3 client migration validation did not complete");
        local_ = migration_local_;
        quic_local_ = migration_local_;
        socket_ = std::move(*migration_socket_);
        migration_socket_.reset();
        flush();
    }

    // Waits for the final head or the stream's reset. The reference stays
    // valid until finish_response() or reset_request() for the stream.
    [[nodiscard]] const http3_test_response& wait_for_response_head(std::int64_t stream_id) {
        auto& stream = tracked_stream(stream_id);
        wait_or_throw([&] { return stream.response_.status_ != 0 || response_complete(stream); },
            "HTTP/3 response head did not arrive");
        return stream.response_;
    }

    // The body bytes received since the last take, at least one, returning
    // their flow credit; nullopt once the response ended or was reset with
    // nothing left to read. Throws when neither happens within `wait`.
    [[nodiscard]] std::optional<std::string> read_body_chunk(
        std::int64_t stream_id, std::chrono::milliseconds wait) {
        auto& stream = tracked_stream(stream_id);
        if (!wait_until([&] { return !stream.unread_body_.empty() || response_complete(stream); }, wait) &&
            stream.unread_body_.empty()) {
            throw std::runtime_error(("HTTP/3 response body did not progress: " + describe_wait_failure()).c_str());
        }
        if (stream.unread_body_.empty()) {
            return std::nullopt;
        }
        auto chunk = std::exchange(stream.unread_body_, std::string{});
        stream.unread_credit_.reset();
        flush();
        return chunk;
    }

    // Waits for the response to complete or be reset and releases the stream.
    // body_ holds the bytes read_body_chunk() has not already taken.
    [[nodiscard]] http3_test_response finish_response(std::int64_t stream_id) {
        auto& stream = tracked_stream(stream_id);
        stream.consume_on_arrival_ = true;
        if (!stream.unread_body_.empty()) {
            stream.response_.body_ += stream.unread_body_;
            stream.unread_credit_.reset();
            stream.unread_body_.clear();
        }
        wait_or_throw([&] { return response_complete(stream); }, "HTTP/3 response did not complete");
        auto response = std::move(stream.response_);
        streams_.erase(stream_id);
        return response;
    }

    // Abandons the request in both directions with RESET_STREAM and
    // STOP_SENDING carrying `application_error` (RFC 9114 §4.1.1).
    void reset_request(std::int64_t stream_id, http3_error_code application_error) {
        static_cast<void>(tracked_stream(stream_id));
        streams_.erase(stream_id);
        connection_->reset_stream(stream_id, application_error);
        flush();
    }

    // Drives the connection until `done()` holds or `wait` elapses. Returns
    // false on the deadline, or once the connection closed while `done()`
    // still does not hold.
    template <typename predicate_type>
    [[nodiscard]] bool wait_until(predicate_type&& done, std::chrono::milliseconds wait) {
        const auto deadline = std::chrono::steady_clock::now() + wait;
        for (;;) {
            if (done()) {
                return true;
            }
            if (connection_->closed() || std::chrono::steady_clock::now() >= deadline) {
                flush();
                return done();
            }
            pump(deadline);
        }
    }

    // Every GOAWAY identifier received, in order; nullopt marks a shutdown
    // notice (RFC 9114 §5.2).
    [[nodiscard]] const std::vector<std::optional<std::int64_t>>& goaway_ids() const noexcept {
        return goaway_ids_;
    }

    // The closing or draining period began, by either endpoint.
    [[nodiscard]] bool closed() const noexcept {
        return connection_->closed();
    }

    // Sends CONNECTION_CLOSE with H3_NO_ERROR (RFC 9114 §5.2).
    void close() {
        connection_->close(http3_error_code::no_error);
        flush();
    }

    // The HTTP/3 or QUIC transport code of the CONNECTION_CLOSE sent or
    // received.
    [[nodiscard]] std::optional<http3_error_code> application_error() const noexcept {
        return connection_->application_error();
    }
    [[nodiscard]] std::optional<std::uint64_t> transport_error() const noexcept {
        return connection_->transport_error();
    }

    [[nodiscard]] bool after_retry() const noexcept {
        return connection_->after_retry();
    }

    // Only for the server's transport parameters and connection IDs, which no
    // public API reports.
    [[nodiscard]] const ngtcp2_conn* native_handle() const noexcept {
        return connection_->native_handle();
    }

private:
    struct stream_state final {
        http3_test_response response_;
        std::string unread_body_{};
        // Receive credit of unread_body_, returned as the test takes it.
        std::optional<http3_received_data_credit> unread_credit_;
        std::vector<std::string> datagrams_{};
        bool consume_on_arrival_{false};
        bool ended_{false};
        bool closed_{false};
    };

    static constexpr std::size_t max_stream_output_bytes = 256 * 1024;
    static constexpr std::size_t max_udp_payload_bytes = 1350;

    static int handshake_confirmed(ngtcp2_conn*, void* user_data) noexcept {
        static_cast<http3_test_client*>(user_data)->handshake_confirmed_ = true;
        return 0;
    }

    static int handshake_complete(ngtcp2_conn*, void* user_data) noexcept {
        auto& self = *static_cast<http3_test_client*>(user_data);
        self.early_rejected_ = self.early_attempted_ &&
                               self.tls_.early_data_status() != SSL_EARLY_DATA_ACCEPTED;
        self.early_rejection_pending_ = self.early_rejected_;
        if (!self.early_rejected_) {
            return 0;
        }
        try {
            return self.connection_->reject_early_data() == http3_quic_status::accepted
                       ? 0
                       : NGTCP2_ERR_CALLBACK_FAILURE;
        } catch (...) {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
    }

    void apply_early_rejection() {
        if (!std::exchange(early_rejection_pending_, false)) {
            return;
        }
        for (auto& [id, stream] : streams_) {
            static_cast<void>(id);
            stream.response_.reset_error_ = http3_error_code::request_rejected;
            stream.closed_ = true;
        }
        if (connection_->start_http3() != http3_quic_status::accepted) {
            throw std::runtime_error("HTTP/3 client cannot restart after early rejection");
        }
    }

    [[nodiscard]] asio::ip::udp::socket replacement_socket() {
        asio::ip::udp::socket replacement(io_);
        replacement.open(server_.protocol());
        replacement.connect(server_);
        replacement.non_blocking(true);
        return replacement;
    }

    [[nodiscard]] static ngtcp2_tstamp timestamp() noexcept {
        return static_cast<ngtcp2_tstamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    [[nodiscard]] ngtcp2_path path(asio::ip::udp::endpoint& local) noexcept {
        return {
            .local = {reinterpret_cast<ngtcp2_sockaddr*>(local.data()), static_cast<ngtcp2_socklen>(local.size())},
            .remote = {reinterpret_cast<ngtcp2_sockaddr*>(server_.data()), static_cast<ngtcp2_socklen>(server_.size())},
            .user_data = nullptr,
        };
    }

    [[nodiscard]] ngtcp2_path path() noexcept {
        return path(quic_local_);
    }

    [[nodiscard]] std::string authority() const {
        std::error_code literal_error;
        const auto address = asio::ip::make_address(server_name_.c_str(), literal_error);
        std::string host(server_name_);
        if (!literal_error && address.is_v6()) {
            host.insert(host.begin(), '[');
            host.push_back(']');
        }
        host.push_back(':');
        host.append(std::to_string(server_.port()));
        return host;
    }

    [[nodiscard]] std::string describe_wait_failure() const {
        if (!connection_->closed()) {
            return std::string("timed out");
        }
        if (const auto error = connection_->application_error()) {
            return "connection closed with HTTP/3 error " +
                   std::string(std::to_string(static_cast<std::uint64_t>(*error)));
        }
        if (const auto error = connection_->transport_error()) {
            return "connection closed with QUIC error " + std::string(std::to_string(*error));
        }
        return std::string("connection closed");
    }

    template <typename predicate_type>
    void wait_or_throw(predicate_type&& done, const char* what) {
        if (!wait_until(std::forward<predicate_type>(done), timeout_)) {
            throw std::runtime_error((std::string(what) + ": " + describe_wait_failure()).c_str());
        }
    }

    [[nodiscard]] stream_state& tracked_stream(std::int64_t stream_id) {
        const auto found = streams_.find(stream_id);
        if (found == streams_.end()) {
            throw std::invalid_argument("HTTP/3 test client: unknown request stream");
        }
        return found->second;
    }

    [[nodiscard]] static bool response_complete(const stream_state& stream) noexcept {
        return stream.ended_ || stream.closed_;
    }

    [[nodiscard]] std::int64_t open_stream(bool consume_on_arrival) {
        std::optional<std::int64_t> opened;
        wait_or_throw([&] {
            opened = connection_->open_request_stream();
            return opened.has_value();
        },
            "HTTP/3 request stream did not open");
        streams_[*opened].consume_on_arrival_ = consume_on_arrival;
        return *opened;
    }

    template <typename head_type>
    [[nodiscard]] std::int64_t open_tunnel(const head_type& request) {
        if constexpr (std::is_same_v<head_type, http3_extended_connect_request_head_view>) {
            // TLS completion does not imply arrival of the server's HTTP/3 SETTINGS.
            (void)peer_settings();
        }
        const auto stream_id = open_stream(false);
        if (connection_->http3().submit_request_head(stream_id, request) != http3_submit_status::accepted) {
            throw std::runtime_error("HTTP/3 tunnel request was not accepted");
        }
        flush();
        return stream_id;
    }

    [[nodiscard]] std::int64_t open_request(std::string_view method, std::string_view request_path,
        std::initializer_list<std::pair<std::string_view, std::string_view>> headers, std::string_view body, bool consume_on_arrival) {
        const auto stream_id = open_stream(consume_on_arrival);
        auto& stream = streams_.at(stream_id);

        const auto request_authority = authority();
        std::vector<http_header_view> fields{};
        fields.reserve(headers.size());
        for (const auto& [name, value] : headers) {
            fields.emplace_back(name, value);
        }
        const http3_regular_request_head_view head{.method_ = method,
            .authority_ = std::optional<borrowed_text>(request_authority),
            .target_ = request_path,
            .headers_ = fields,
            .content_ = body.empty() ? http_request_content_plan::none() : http_request_content_plan::known_length(body.size())};
        if (connection_->http3().submit_request_head(stream_id, head) != http3_submit_status::accepted) {
            throw std::runtime_error("HTTP/3 test client cannot submit the request head");
        }
        if (!body.empty()) {
            send_body(stream_id, stream, std::as_bytes(std::span(body.data(), body.size())));
        }
        flush();
        return stream_id;
    }

    void send_body(std::int64_t stream_id, const stream_state& stream, std::span<const std::byte> body) {
        auto& codec = connection_->http3();
        while (!body.empty()) {
            const auto submitted = codec.submit_data(stream_id, body);
            body = body.subspan(submitted.consumed_bytes_);
            if (submitted.status_ == http3_data_submit_status::complete || submitted.consumed_bytes_ != 0) {
                continue;
            }
            if (submitted.status_ != http3_data_submit_status::backpressured) {
                // The server stopped the upload (STOP_SENDING or a reset);
                // its response or reset is still awaited.
                return;
            }
            // The stream's output budget is full until the server acknowledges
            // DATA; an early complete response also ends the upload.
            wait_or_throw([&] {
                return codec.pending_output_bytes(stream_id) < max_stream_output_bytes || response_complete(stream);
            },
                "HTTP/3 request body was not acknowledged");
            if (response_complete(stream)) {
                return;
            }
        }
        if (codec.finish_stream(stream_id) != http3_finish_status::accepted && !response_complete(stream)) {
            throw std::runtime_error("HTTP/3 test client cannot finish the request body");
        }
    }

    void pump(std::chrono::steady_clock::time_point deadline) {
        flush();
        auto wake = deadline;
        const auto expiry = connection_->expiry();
        if (expiry < static_cast<ngtcp2_tstamp>((std::numeric_limits<std::int64_t>::max)())) {
            const std::chrono::steady_clock::time_point timer(
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::nanoseconds(static_cast<std::int64_t>(expiry))));
            wake = (std::min)(wake, timer);
        }
        wait_readable(wake);
        receive_available();
        if (!connection_->closed() && timestamp() >= connection_->expiry()) {
            static_cast<void>(connection_->handle_expiry(timestamp()));
            drain_events();
        }
        flush();
    }

    void wait_readable(std::chrono::steady_clock::time_point until) {
        if (until <= std::chrono::steady_clock::now()) {
            return;
        }
        socket_.async_wait(asio::socket_base::wait_read, [](const std::error_code&) {});
        if (migration_socket_) {
            migration_socket_->async_wait(asio::socket_base::wait_read, [](const std::error_code&) {});
        }
        io_.restart();
        io_.run_one_until(until);
        socket_.cancel();
        if (migration_socket_) {
            migration_socket_->cancel();
        }
        io_.restart();
        io_.run();
    }

    void receive_available() {
        receive_available(socket_, quic_local_);
        if (migration_socket_) {
            receive_available(*migration_socket_, migration_local_);
        }
    }

    void receive_available(asio::ip::udp::socket& socket, asio::ip::udp::endpoint& local) {
        for (;;) {
            std::error_code error;
            const auto size = socket.receive(asio::buffer(input_), 0, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                return;
            }
            if (error) {
                throw std::system_error(error, "HTTP/3 test client UDP receive failed");
            }
            if (connection_->closed()) {
                continue;
            }
            static_cast<void>(connection_->receive_datagram(path(local), NGTCP2_ECN_NOT_ECT,
                std::span<const std::byte>(input_.data(), size), timestamp()));
            apply_early_rejection();
            drain_events();
        }
    }

    void flush() {
        for (;;) {
            const auto result = connection_->next_datagram(max_udp_payload_bytes, timestamp(), output_);
            drain_events();
            if (result.status_ != http3_quic_status::accepted || output_.bytes_.empty()) {
                return;
            }
            auto* output_socket = &socket_;
            if (migration_socket_) {
                const auto candidate = path(migration_local_);
                if (ngtcp2_path_eq(&output_.path_.path, &candidate) != 0) {
                    output_socket = &*migration_socket_;
                }
            }
            for (;;) {
                std::error_code error;
                output_socket->send(asio::buffer(output_.bytes_.data(), output_.bytes_.size()), 0, error);
                if (!error) {
                    break;
                }
                if (error != asio::error::would_block && error != asio::error::try_again) {
                    throw std::system_error(error, "HTTP/3 test client UDP send failed");
                }
                output_socket->wait(asio::socket_base::wait_write, error);
            }
        }
    }

    void drain_events() {
        while (auto event = connection_->next_event()) {
            apply(*event);
        }
    }

    void apply(http3_event& event) {
        if (const auto* datagram = event.datagram()) {
            if (const auto found = streams_.find(datagram->stream_id()); found != streams_.end()) {
                found->second.datagrams_.emplace_back(datagram->payload());
            }
        } else if (const auto* goaway = event.goaway()) {
            goaway_ids_.push_back(goaway->id());
        } else if (auto* data = event.data()) {
            // A released stream's late DATA returns its credit with the event.
            const auto found = streams_.find(data->stream_id());
            if (found == streams_.end()) {
                return;
            }
            auto& stream = found->second;
            if (stream.consume_on_arrival_) {
                stream.response_.body_.append(data->bytes());
                return;
            }
            stream.unread_body_.append(data->bytes());
            if (auto credit = data->take_credit(); credit.valid()) {
                if (stream.unread_credit_) {
                    stream.unread_credit_->merge(std::move(credit));
                } else {
                    stream.unread_credit_.emplace(std::move(credit));
                }
            }
        } else if (const auto* interim = event.informational_head()) {
            if (const auto found = streams_.find(interim->stream_id()); found != streams_.end()) {
                found->second.response_.interim_statuses_.push_back(interim->head().received_status().value());
            }
        } else if (const auto* head = event.response_head()) {
            if (const auto found = streams_.find(head->stream_id()); found != streams_.end()) {
                auto& stream = found->second;
                stream.response_.status_ = head->head().status().value();
                stream.response_.headers_ = copy_fields(head->head().headers());
            }
        } else if (const auto* end = event.message_end()) {
            if (const auto found = streams_.find(end->stream_id()); found != streams_.end()) {
                found->second.response_.trailers_ = copy_fields(end->trailers());
                found->second.ended_ = true;
            }
        } else if (const auto* action = event.transport_action()) {
            // Local actions are codec instructions the connection already
            // executed, and a peer RESET_STREAM is not completion either:
            // the message end and the stream close own it.
            if (action->action() == http3_transport_action::stop_sending &&
                action->origin() == http3_transport_action_origin::peer) {
                if (const auto found = streams_.find(action->stream_id()); found != streams_.end()) {
                    found->second.response_.stop_sending_error_ = action->error();
                }
            }
        } else if (const auto* unprocessed = event.request_unprocessed()) {
            // No later event names the stream.
            if (const auto found = streams_.find(unprocessed->stream_id()); found != streams_.end()) {
                found->second.response_.unprocessed_ = true;
                found->second.closed_ = true;
            }
        } else if (const auto* closed = event.stream_close()) {
            if (const auto found = streams_.find(closed->stream_id()); found != streams_.end()) {
                found->second.closed_ = true;
                auto& reset_error = found->second.response_.reset_error_;
                if (!reset_error) {
                    reset_error = closed->error();
                }
            }
        }
    }

    [[nodiscard]] static http3_test_fields copy_fields(std::span<const http_header> source) {
        http3_test_fields fields{};
        fields.reserve(source.size());
        for (const auto& field : source) {
            fields.emplace_back(field.name(), field.value());
        }
        return fields;
    }

    std::chrono::milliseconds timeout_;
    std::string server_name_{};
    asio::ip::udp::endpoint server_;
    asio::ip::udp::endpoint local_;
    // The local address the QUIC connection uses for its path; rebind()
    // changes only the socket, as a NAT would.
    asio::ip::udp::endpoint quic_local_;
    asio::io_context io_;
    asio::ip::udp::socket socket_;
    std::optional<asio::ip::udp::socket> migration_socket_;
    asio::ip::udp::endpoint migration_local_;
    http3_test_client_tls tls_;
    // Destroyed before the TLS session it uses.
    std::optional<http3_quic_connection> connection_;
    http3_quic_datagram output_;
    std::vector<std::byte> input_{};
    std::map<std::int64_t, stream_state> streams_{};
    std::vector<std::optional<std::int64_t>> goaway_ids_{};
    bool early_attempted_{false};
    bool early_rejected_{false};
    bool early_rejection_pending_{false};
    bool handshake_confirmed_{false};
};

}  // namespace ruvia::test

namespace ruvia::test {

// Blocking HTTP/3 client for malformed-request wire regressions. It drives
// ngtcp2 and nghttp3 directly, so a request field section reaches the wire
// verbatim: ruvia's public HTTP/3 API validates every request head and never
// sends one. One request is in flight at a time; DATA credit returns at once.
class http3_raw_request_client final {
public:
    http3_raw_request_client(asio::ip::udp::endpoint server, const std::filesystem::path& ca_file,
        std::string_view server_name, std::chrono::milliseconds timeout)
        : timeout_(timeout),
          server_(server),
          socket_(io_),
          tls_(ca_file, server_name) {
        socket_.open(server_.protocol());
        socket_.connect(server_);
        socket_.non_blocking(true);
        local_ = socket_.local_endpoint();
        // This client owns ngtcp2 itself, so it installs the adapter's crypto
        // callbacks next to its own stream callbacks.
        ngtcp2_callbacks callbacks{};
        callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
        callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
        callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
        callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
        callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
        callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;
        callbacks.update_key = ngtcp2_crypto_update_key_cb;
        callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
        callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
        callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
        callbacks.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
        callbacks.rand = http3_test_fill_random;
        callbacks.get_new_connection_id2 = http3_test_new_connection_id;
        callbacks.handshake_completed = [](ngtcp2_conn*, void* user_data) noexcept {
            static_cast<http3_raw_request_client*>(user_data)->handshake_completed_ = true;
            return 0;
        };
        callbacks.recv_stream_data = receive_stream_data;
        callbacks.acked_stream_data_offset = acknowledge_stream_data;
        callbacks.stream_close2 = close_stream;
        callbacks.stream_reset = reset_stream;
        callbacks.extend_max_stream_data = extend_stream_data;
        ngtcp2_settings settings;
        ngtcp2_settings_default(&settings);
        settings.initial_ts = timestamp();
        settings.max_tx_udp_payload_size = max_udp_payload_bytes;
        ngtcp2_transport_params parameters;
        ngtcp2_transport_params_default(&parameters);
        parameters.initial_max_data = 16 * 1024 * 1024;
        parameters.initial_max_stream_data_bidi_local = 1024 * 1024;
        parameters.initial_max_stream_data_uni = 1024 * 1024;
        parameters.initial_max_streams_uni = 16;
        parameters.max_idle_timeout = 30 * NGTCP2_SECONDS;
        const auto source = http3_test_random_connection_id(16);
        const auto destination = http3_test_random_connection_id(18);
        const auto initial_path = path();
        if (ngtcp2_conn_client_new(&connection_, &destination, &source, &initial_path, NGTCP2_PROTO_VER_V1,
                &callbacks, &settings, &parameters, nullptr, this) != 0) {
            throw std::runtime_error("HTTP/3 raw client cannot create its QUIC connection");
        }
        tls_.bind(get_connection, this, connection_);
        if (!wait_until([this] { return handshake_completed_ && start_http3(); })) {
            throw std::runtime_error("HTTP/3 raw client handshake failed");
        }
    }

    ~http3_raw_request_client() {
        if (http3_ != nullptr) {
            nghttp3_conn_del(http3_);
        }
        ngtcp2_conn_del(connection_);
    }

    http3_raw_request_client(const http3_raw_request_client&) = delete;
    http3_raw_request_client& operator=(const http3_raw_request_client&) = delete;
    http3_raw_request_client(http3_raw_request_client&&) = delete;
    http3_raw_request_client& operator=(http3_raw_request_client&&) = delete;

    // Sends `fields` verbatim as the request field section, after the peer's
    // SETTINGS arrived. end_stream=false leaves the request send half open.
    // Returns once the response ended or the stream closed or was reset.
    [[nodiscard]] http3_test_response request(
        std::initializer_list<std::pair<std::string_view, std::string_view>> fields, bool end_stream = true) {
        wait_or_throw([this] { return settings_received_; }, "HTTP/3 raw client did not receive SETTINGS");
        std::int64_t stream_id = -1;
        wait_or_throw([&] { return ngtcp2_conn_open_bidi_stream(connection_, &stream_id, nullptr) == 0; },
            "HTTP/3 raw client cannot open a request stream");
        std::vector<nghttp3_nv> native;
        native.reserve(fields.size());
        for (const auto& [name, value] : fields) {
            native.push_back({
                .name = reinterpret_cast<std::uint8_t*>(const_cast<char*>(name.data())),
                .value = reinterpret_cast<std::uint8_t*>(const_cast<char*>(value.data())),
                .namelen = name.size(),
                .valuelen = value.size(),
                .flags = NGHTTP3_NV_FLAG_NONE,
            });
        }
        const nghttp3_data_reader open_send_half{.read_data = provide_no_content};
        if (nghttp3_conn_submit_request(http3_, stream_id, native.data(), native.size(),
                end_stream ? nullptr : &open_send_half, nullptr) != 0) {
            throw std::runtime_error("HTTP/3 raw client cannot submit the field section");
        }
        stream_id_ = stream_id;
        response_ = {};
        complete_ = false;
        wait_or_throw([this] { return complete_; }, "HTTP/3 raw response did not complete");
        stream_id_ = -1;
        return std::move(response_);
    }

    // The connection failed or entered its closing or draining period.
    [[nodiscard]] bool closed() const noexcept {
        return failed_;
    }

    // For assertions on the CONNECTION_CLOSE the peer sent.
    [[nodiscard]] const ngtcp2_conn* native_handle() const noexcept {
        return connection_;
    }

private:
    static constexpr std::size_t max_udp_payload_bytes = 1350;

    [[nodiscard]] static http3_raw_request_client& self(void* user_data) noexcept {
        return *static_cast<http3_raw_request_client*>(user_data);
    }

    // An open request send half has no content to provide yet.
    static nghttp3_ssize provide_no_content(nghttp3_conn*, std::int64_t, nghttp3_vec*, std::size_t, std::uint32_t*,
        void*, void*) noexcept {
        return NGHTTP3_ERR_WOULDBLOCK;
    }

    static ngtcp2_conn* get_connection(ngtcp2_crypto_conn_ref* reference) noexcept {
        return self(reference->user_data).connection_;
    }

    [[nodiscard]] static ngtcp2_tstamp timestamp() noexcept {
        return static_cast<ngtcp2_tstamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    [[nodiscard]] ngtcp2_path path() noexcept {
        return {
            .local = {reinterpret_cast<ngtcp2_sockaddr*>(local_.data()), static_cast<ngtcp2_socklen>(local_.size())},
            .remote = {reinterpret_cast<ngtcp2_sockaddr*>(server_.data()), static_cast<ngtcp2_socklen>(server_.size())},
            .user_data = nullptr,
        };
    }

    // Binds the HTTP/3 control and QPACK streams once QUIC permits them.
    [[nodiscard]] bool start_http3() {
        if (http3_ != nullptr) {
            return true;
        }
        if (ngtcp2_conn_get_streams_uni_left2(connection_) < 3) {
            return false;
        }
        nghttp3_callbacks callbacks{};
        callbacks.acked_stream_data = nullptr;
        callbacks.recv_header = receive_header;
        callbacks.recv_trailer = receive_header;
        callbacks.recv_data = [](nghttp3_conn*, std::int64_t stream_id, const std::uint8_t* data, std::size_t size,
                                  void* user_data, void*) noexcept {
            auto& client = self(user_data);
            if (stream_id == client.stream_id_) {
                client.response_.body_.append(reinterpret_cast<const char*>(data), size);
            }
            return 0;
        };
        callbacks.end_stream = [](nghttp3_conn*, std::int64_t stream_id, void* user_data, void*) noexcept {
            auto& client = self(user_data);
            client.complete_ = client.complete_ || stream_id == client.stream_id_;
            return 0;
        };
        callbacks.stop_sending = [](nghttp3_conn*, std::int64_t stream_id, std::uint64_t error, void* user_data,
                                     void*) noexcept {
            (void)ngtcp2_conn_shutdown_stream_read(self(user_data).connection_, 0, stream_id, error);
            return 0;
        };
        callbacks.reset_stream = [](nghttp3_conn*, std::int64_t stream_id, std::uint64_t error, void* user_data,
                                     void*) noexcept {
            (void)ngtcp2_conn_shutdown_stream_write(self(user_data).connection_, 0, stream_id, error);
            return 0;
        };
        callbacks.recv_settings2 = [](nghttp3_conn*, const nghttp3_proto_settings*, void* user_data) noexcept {
            self(user_data).settings_received_ = true;
            return 0;
        };
        nghttp3_settings settings;
        nghttp3_settings_default(&settings);
        if (nghttp3_conn_client_new(&http3_, &callbacks, &settings, nullptr, this) != 0) {
            throw std::runtime_error("HTTP/3 raw client cannot create its HTTP/3 connection");
        }
        std::array<std::int64_t, 3> streams{};
        for (auto& stream_id : streams) {
            if (ngtcp2_conn_open_uni_stream(connection_, &stream_id, nullptr) != 0) {
                throw std::runtime_error("HTTP/3 raw client cannot open its control streams");
            }
        }
        if (nghttp3_conn_bind_control_stream(http3_, streams[0]) != 0 ||
            nghttp3_conn_bind_qpack_streams(http3_, streams[1], streams[2]) != 0) {
            throw std::runtime_error("HTTP/3 raw client cannot bind its control streams");
        }
        return true;
    }

    static int receive_header(nghttp3_conn*, std::int64_t stream_id, std::int32_t, nghttp3_rcbuf* name,
        nghttp3_rcbuf* value, std::uint8_t, void* user_data, void*) noexcept {
        auto& client = self(user_data);
        if (stream_id != client.stream_id_) {
            return 0;
        }
        const auto name_buffer = nghttp3_rcbuf_get_buf(name);
        const auto value_buffer = nghttp3_rcbuf_get_buf(value);
        const std::string_view name_view(reinterpret_cast<const char*>(name_buffer.base), name_buffer.len);
        const std::string_view value_view(reinterpret_cast<const char*>(value_buffer.base), value_buffer.len);
        try {
            if (name_view == ":status") {
                client.response_.status_ = 0;
                for (const char digit : value_view) {
                    client.response_.status_ = client.response_.status_ * 10 + (digit - '0');
                }
                if (client.response_.status_ < 200) {
                    client.response_.interim_statuses_.push_back(client.response_.status_);
                    client.response_.status_ = 0;
                }
            } else if (client.response_.status_ != 0) {
                client.response_.headers_.emplace_back(name_view, value_view);
            }
        } catch (...) {
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
        return 0;
    }

    static int receive_stream_data(ngtcp2_conn* connection, std::uint32_t flags, std::int64_t stream_id,
        std::uint64_t, const std::uint8_t* data, std::size_t size, void* user_data, void*) noexcept {
        auto& client = self(user_data);
        if (client.http3_ == nullptr ||
            nghttp3_conn_read_stream2(client.http3_, stream_id, data, size, (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0,
                ngtcp2_conn_get_timestamp(connection)) < 0) {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
        // Every received byte is consumed at once.
        ngtcp2_conn_extend_max_stream_offset(connection, stream_id, size);
        ngtcp2_conn_extend_max_offset(connection, size);
        return 0;
    }

    static int acknowledge_stream_data(ngtcp2_conn*, std::int64_t stream_id, std::uint64_t, std::uint64_t size,
        void* user_data, void*) noexcept {
        auto& client = self(user_data);
        return client.http3_ == nullptr || nghttp3_conn_add_ack_offset(client.http3_, stream_id, size) == 0
                   ? 0
                   : NGTCP2_ERR_CALLBACK_FAILURE;
    }

    static int close_stream(ngtcp2_conn*, std::uint32_t flags, std::int64_t stream_id, std::uint64_t receive_error,
        std::uint64_t send_error, void* user_data, void*) noexcept {
        auto& client = self(user_data);
        if (stream_id == client.stream_id_) {
            if ((flags & NGTCP2_STREAM_CLOSE2_FLAG_RX_APP_ERROR_CODE_SET) != 0 && receive_error != NGHTTP3_H3_NO_ERROR &&
                !client.response_.reset_error_) {
                client.response_.reset_error_ = static_cast<http3_error_code>(receive_error);
            }
            client.complete_ = true;
        }
        if (client.http3_ == nullptr) {
            return 0;
        }
        std::uint32_t native_flags = 0;
        if ((flags & NGTCP2_STREAM_CLOSE2_FLAG_RX_APP_ERROR_CODE_SET) != 0) {
            native_flags |= NGHTTP3_STREAM_CLOSE_FLAG_RX_APP_ERROR_CODE_SET;
        }
        if ((flags & NGTCP2_STREAM_CLOSE2_FLAG_TX_APP_ERROR_CODE_SET) != 0) {
            native_flags |= NGHTTP3_STREAM_CLOSE_FLAG_TX_APP_ERROR_CODE_SET;
        }
        const int result = nghttp3_conn_close_stream2(client.http3_, native_flags, stream_id, receive_error, send_error);
        return result == 0 || result == NGHTTP3_ERR_STREAM_NOT_FOUND ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
    }

    static int reset_stream(ngtcp2_conn*, std::int64_t stream_id, std::uint64_t, std::uint64_t error,
        void* user_data, void*) noexcept {
        auto& client = self(user_data);
        if (stream_id == client.stream_id_ && !client.response_.reset_error_) {
            client.response_.reset_error_ = static_cast<http3_error_code>(error);
        }
        return client.http3_ == nullptr || nghttp3_conn_shutdown_stream_read(client.http3_, stream_id) == 0
                   ? 0
                   : NGTCP2_ERR_CALLBACK_FAILURE;
    }

    static int extend_stream_data(ngtcp2_conn*, std::int64_t stream_id, std::uint64_t, void* user_data,
        void*) noexcept {
        auto& client = self(user_data);
        return client.http3_ == nullptr || nghttp3_conn_unblock_stream(client.http3_, stream_id) == 0
                   ? 0
                   : NGTCP2_ERR_CALLBACK_FAILURE;
    }

    template <typename predicate_type>
    [[nodiscard]] bool wait_until(predicate_type&& done) {
        const auto deadline = std::chrono::steady_clock::now() + timeout_;
        for (;;) {
            if (done()) {
                return true;
            }
            if (failed_ || std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            pump(deadline);
        }
    }

    template <typename predicate_type>
    void wait_or_throw(predicate_type&& done, const char* what) {
        if (!wait_until(std::forward<predicate_type>(done))) {
            throw std::runtime_error(what);
        }
    }

    void pump(std::chrono::steady_clock::time_point deadline) {
        flush();
        auto wake = deadline;
        const auto expiry = ngtcp2_conn_get_expiry(connection_);
        if (expiry < static_cast<ngtcp2_tstamp>((std::numeric_limits<std::int64_t>::max)())) {
            wake = (std::min)(wake, std::chrono::steady_clock::time_point(
                                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                            std::chrono::nanoseconds(static_cast<std::int64_t>(expiry)))));
        }
        if (wake > std::chrono::steady_clock::now()) {
            socket_.async_wait(asio::socket_base::wait_read, [](const std::error_code&) {});
            io_.restart();
            io_.run_one_until(wake);
            socket_.cancel();
            io_.restart();
            io_.run();
        }
        receive_available();
        if (!failed_ && timestamp() >= ngtcp2_conn_get_expiry(connection_)) {
            failed_ = ngtcp2_conn_handle_expiry(connection_, timestamp()) != 0;
        }
        flush();
    }

    void receive_available() {
        for (;;) {
            std::error_code error;
            const auto size = socket_.receive(asio::buffer(input_), 0, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                return;
            }
            if (error) {
                throw std::system_error(error, "HTTP/3 raw client UDP receive failed");
            }
            if (failed_) {
                continue;
            }
            const auto received_path = path();
            const ngtcp2_pkt_info info{};
            failed_ = ngtcp2_conn_read_pkt(connection_, &received_path, &info,
                          reinterpret_cast<const std::uint8_t*>(input_.data()), size, timestamp()) != 0;
        }
    }

    void flush() {
        if (failed_) {
            return;
        }
        for (;;) {
            std::array<nghttp3_vec, 16> vectors{};
            std::int64_t stream_id = -1;
            int fin = 0;
            nghttp3_ssize count = 0;
            if (http3_ != nullptr) {
                count = nghttp3_conn_writev_stream(http3_, &stream_id, &fin, vectors.data(), vectors.size());
                if (count < 0) {
                    failed_ = true;
                    return;
                }
            }
            std::uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
            if (fin != 0) {
                flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
            }
            ngtcp2_path_storage storage;
            ngtcp2_path_storage_zero(&storage);
            ngtcp2_pkt_info info{};
            ngtcp2_ssize written = -1;
            const auto size = ngtcp2_conn_writev_stream(connection_, &storage.path, &info,
                reinterpret_cast<std::uint8_t*>(output_.data()), output_.size(), &written, flags, stream_id,
                reinterpret_cast<const ngtcp2_vec*>(vectors.data()), static_cast<std::size_t>(count), timestamp());
            if (size < 0) {
                if (size == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
                    nghttp3_conn_block_stream(http3_, stream_id);
                    continue;
                }
                if (size == NGTCP2_ERR_STREAM_SHUT_WR) {
                    nghttp3_conn_shutdown_stream_write(http3_, stream_id);
                    continue;
                }
                if (size == NGTCP2_ERR_WRITE_MORE) {
                    if (nghttp3_conn_add_write_offset(http3_, stream_id, static_cast<std::size_t>(written)) != 0) {
                        failed_ = true;
                        return;
                    }
                    continue;
                }
                failed_ = true;
                return;
            }
            if (written >= 0 && nghttp3_conn_add_write_offset(http3_, stream_id, static_cast<std::size_t>(written)) != 0) {
                failed_ = true;
                return;
            }
            if (size == 0) {
                return;
            }
            std::error_code error;
            socket_.send(asio::buffer(output_.data(), static_cast<std::size_t>(size)), 0, error);
            if (error && error != asio::error::would_block && error != asio::error::try_again) {
                throw std::system_error(error, "HTTP/3 raw client UDP send failed");
            }
        }
    }

    std::chrono::milliseconds timeout_;
    asio::ip::udp::endpoint server_;
    asio::ip::udp::endpoint local_;
    asio::io_context io_;
    asio::ip::udp::socket socket_;
    http3_test_client_tls tls_;
    ngtcp2_conn* connection_{nullptr};
    nghttp3_conn* http3_{nullptr};
    std::array<std::byte, max_udp_payload_bytes> output_{};
    std::vector<std::byte> input_ = std::vector<std::byte>(64 * 1024);
    http3_test_response response_;
    std::int64_t stream_id_{-1};
    bool handshake_completed_{false};
    bool settings_received_{false};
    bool complete_{false};
    bool failed_{false};
};

}  // namespace ruvia::test

#endif

// Per-operation timeouts and stop tokens of the public outbound client, driven
// against the application's own routes over each protocol's real transport.
namespace ruvia::test::client_operations {

using namespace std::chrono_literals;

// Each route stalls where a client operation is interrupted, far longer than
// that interruption, then completes so an interrupted read can resume.
inline constexpr auto server_stall = 300ms;
inline constexpr auto interruption = 30ms;

class routes final {
public:
    RUVIA_ROUTES_BEGIN(routes)
    RUVIA_GET("/operation/stalled-body", stalled_body);
    RUVIA_POST("/operation/held-upload", held_upload, ruvia::stream_body);
    RUVIA_POST("/operation/echo-upload", echo_upload, ruvia::stream_body);
    RUVIA_POST("/operation/late-head", late_head, ruvia::stream_body);
    RUVIA_CONNECT("operation.test:443", echo_tunnel);
    RUVIA_ROUTES_END

    ruvia::task<void> stalled_body(ruvia::context& context) {
        auto& writer = context.stream_text();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write("first-");
        (void)co_await writer.sleep(server_stall);
        co_await writer.write("second");
        co_await writer.end();
    }
    // Never reads its content, so a 100-continue upload is never released.
    ruvia::task<ruvia::http_response> held_upload(ruvia::context& context) {
        (void)co_await ruvia::sleep_for(context.worker(), 1s, context.stop_token());
        co_return context.text("held");
    }
    ruvia::task<ruvia::http_response> echo_upload(ruvia::context& context) {
        std::string echoed("echo:");
        auto& reader = context.req().body_reader();
        while (auto chunk = co_await reader.text()) {
            echoed.append(*chunk);
        }
        co_return context.text(echoed);
    }
    ruvia::task<ruvia::http_response> late_head(ruvia::context& context) {
        auto& reader = context.req().body_reader();
        while (co_await reader.read()) {
        }
        (void)co_await ruvia::sleep_for(context.worker(), server_stall, context.stop_token());
        co_return context.text("late");
    }
    ruvia::task<void> echo_tunnel(ruvia::context& context) {
        auto& tunnel = context.tunnel();
        while (auto chunk = co_await tunnel.read()) {
            co_await tunnel.write(std::move(*chunk));
        }
        co_await tunnel.finish();
    }
};

template <typename value_type>
[[nodiscard]] ruvia::task<std::optional<std::error_code>> failure_of(ruvia::task<value_type> operation) {
    try {
        (void)co_await std::move(operation);
    } catch (const ruvia::http_client_error& error) {
        co_return error.code();
    }
    co_return std::nullopt;
}

[[nodiscard]] inline ruvia::task<void> stop_after(const ruvia::worker_ref& worker, ruvia::stop_source& stop) {
    (void)co_await ruvia::sleep_for(worker, interruption);
    stop.request_stop();
}

inline const auto timed_out = std::make_error_code(std::errc::timed_out);
inline const auto cancelled = std::make_error_code(std::errc::operation_canceled);

inline void append(std::string& target, const std::optional<std::span<const std::byte>>& chunk) {
    if (chunk) {
        target.append(reinterpret_cast<const char*>(chunk->data()), chunk->size());
    }
}

// A read timeout, a stop request and an already-stopped token each end only
// their own wait: the stalled remainder still arrives intact afterwards.
[[nodiscard]] inline ruvia::task<void> check_body_reads(ruvia::http_client_ref client, const ruvia::worker_ref& worker) {
    auto response = co_await client.get("/operation/stalled-body").send();
    std::string body;
    append(body, co_await response.read());
    EXPECT_EQ(body, "first-");
    EXPECT_TRUE(co_await failure_of(response.read({.timeout_ = interruption})) == timed_out);
    ruvia::stop_source stop;
    ruvia::task_scope stopper(worker);
    stopper.spawn(stop_after(worker, stop));
    EXPECT_TRUE(co_await failure_of(response.stream().text({.stop_token_ = stop.token()})) == cancelled);
    co_await stopper.join();
    EXPECT_TRUE(co_await failure_of(response.read({.stop_token_ = stop.token()})) == cancelled);
    EXPECT_THROW((void)response.read({.timeout_ = 0ms}), std::invalid_argument);
    while (auto chunk = co_await response.read()) {
        append(body, chunk);
    }
    EXPECT_EQ(body, "first-second");
}

// An interrupted collection keeps what it received for the next one.
[[nodiscard]] inline ruvia::task<void> check_body_collection(ruvia::http_client_ref client) {
    auto response = co_await client.get("/operation/stalled-body").send();
    EXPECT_TRUE(co_await failure_of(response.bytes(ruvia::default_max_buffered_body_bytes, {.timeout_ = interruption})) == timed_out);
    const auto rest = co_await response.bytes();
    EXPECT_EQ(rest.text(), "first-second");
}

// An upload write that cannot complete in time, or is stopped, aborts the
// exchange: no further content is accepted and no response follows.
[[nodiscard]] inline ruvia::task<void> check_upload_abort(
    ruvia::http_client_ref client, const ruvia::worker_ref& worker, bool cancel) {
    auto exchange = co_await client.post("/operation/held-upload")
                        .upload({.expectation_ = ruvia::http_client_request_expectation::continue_value,
                            .continue_timeout_ = 10s});
    ruvia::stop_source stop;
    ruvia::task_scope stopper(worker);
    ruvia::operation_options operation{.timeout_ = interruption};
    if (cancel) {
        operation = {.stop_token_ = stop.token()};
        stopper.spawn(stop_after(worker, stop));
    }
    EXPECT_TRUE(co_await failure_of(exchange.body().write("held", std::move(operation))) == (cancel ? cancelled : timed_out));
    co_await stopper.join();
    EXPECT_THROW((void)exchange.body().write("more"), ruvia::http_client_error);
    EXPECT_TRUE((co_await failure_of(exchange.response())).has_value());
}

// An already-stopped token sends nothing and leaves the upload usable.
[[nodiscard]] inline ruvia::task<void> check_stopped_upload(ruvia::http_client_ref client) {
    ruvia::stop_source stop;
    stop.request_stop();
    auto exchange = co_await client.post("/operation/echo-upload").upload({});
    EXPECT_TRUE(co_await failure_of(exchange.body().write("lost", {.stop_token_ = stop.token()})) == cancelled);
    EXPECT_THROW((void)exchange.body().write("lost", {.timeout_ = 0ms}), std::invalid_argument);
    co_await exchange.body().write("kept");
    EXPECT_TRUE(co_await failure_of(exchange.body().end({}, {.stop_token_ = stop.token()})) == cancelled);
    co_await exchange.body().end();
    auto response = co_await exchange.response();
    const auto echoed = co_await response.text();
    EXPECT_EQ(echoed.text(), "echo:kept");
}

// Waiting for the final head is a read: its timeout ends only that wait.
[[nodiscard]] inline ruvia::task<void> check_response_wait(ruvia::http_client_ref client) {
    auto exchange = co_await client.post("/operation/late-head").upload({});
    co_await exchange.body().end();
    EXPECT_TRUE(co_await failure_of(exchange.response({.timeout_ = interruption})) == timed_out);
    auto response = co_await exchange.response();
    const auto text = co_await response.text();
    EXPECT_EQ(text.text(), "late");
}

// A tunnel read timeout keeps the tunnel; stopped output sends nothing.
[[nodiscard]] inline ruvia::task<void> check_tunnel(ruvia::http_client_ref client) {
    auto opened = co_await client.connect("operation.test:443").open();
    auto* tunnel = opened.tunnel();
    EXPECT_TRUE(tunnel != nullptr);
    if (tunnel == nullptr) {
        co_return;
    }
    EXPECT_TRUE(co_await failure_of(tunnel->read({.timeout_ = interruption})) == timed_out);
    ruvia::stop_source stop;
    stop.request_stop();
    EXPECT_TRUE(co_await failure_of(tunnel->write("lost", {.stop_token_ = stop.token()})) == cancelled);
    EXPECT_TRUE(co_await failure_of(tunnel->finish({.stop_token_ = stop.token()})) == cancelled);
    EXPECT_THROW((void)tunnel->text({.timeout_ = 0ms}), std::invalid_argument);
    co_await tunnel->write("ping");
    co_await tunnel->finish();
    std::string echoed;
    while (auto chunk = co_await tunnel->text()) {
        echoed.append(*chunk);
    }
    EXPECT_EQ(echoed, "ping");
}

[[nodiscard]] inline ruvia::task<void> exercise(ruvia::http_client_ref client, const ruvia::worker_ref& worker) {
    co_await check_body_reads(client, worker);
    co_await check_body_collection(client);
    co_await check_upload_abort(client, worker, false);
    co_await check_upload_abort(client, worker, true);
    co_await check_stopped_upload(client);
    co_await check_response_wait(client);
    co_await check_tunnel(client);
}

}  // namespace ruvia::test::client_operations

namespace ruvia::test {

// Connection startup, scripted PostgreSQL results and an
// optional transport failure are enough to exercise public backend operations
// without injecting a backend object or private storage.
class backend_startup_peer final {
public:
    enum class protocol { postgresql,
        mariadb,
        redis };
    // What a database peer does once the client is authenticated.
    enum class after_startup {
        // Reads and discards everything the client sends.
        idle,
        // Answers the first statement with the start of a one-column row set
        // and one row, then drops the connection before the result ends.
        drop_mid_result,
        // Like idle, but when the client closes the connection, accepts one
        // more and serves the remaining scripted replies on it.
        reaccept,
    };

    enum class postgres_copy_direction { none,
        input,
        output };

    struct postgres_reply final {
        std::string column_{"v"};
        std::uint32_t type_{25};
        std::vector<std::string> rows_;
        // Marks NULL fields in the single-column rows_ representation.
        std::vector<bool> null_rows_;
        std::string command_{"SELECT 1"};
        char status_{'I'};
        bool deferred_{false};
        // Non-empty: Bind is answered with ErrorResponse carrying this
        // message, sql_state_ and, when non-empty, routine_ instead of a result.
        std::string error_;
        std::string sql_state_{"42601"};
        std::string routine_;
        // COPY keeps the connection in transfer state; no ReadyForQuery follows.
        postgres_copy_direction copy_{postgres_copy_direction::none};
        // Flights read before anything is written: this reply answers the
        // first and the replies after it answer the others, in one write, so a
        // pipelining client must have sent them all before its first reply.
        std::size_t batch_{1};
        // After reading the batch, close the connection instead of answering.
        bool drop_{false};
        // After answering, close the connection while the client holds it
        // idle, as a server ending an idle session does.
        bool close_after_{false};
        // The flight's Parse fails with error_ and keeps no statement, instead
        // of its Bind failing.
        bool parse_fails_{false};
        // Nonzero: held, like deferred_, until release_hold() names it.
        std::size_t hold_{0};
        // Non-empty: multi-column text metadata and row values replace column_/rows_.
        std::vector<std::string> columns_;
        std::vector<std::vector<std::string>> values_;
        // Sends the first row before deferred_/hold_ gates the remaining rows.
        bool first_row_before_hold_{false};
    };

    // One extended-protocol Parse message: the statement text, the parameter
    // type OIDs the client declared (0 leaves a type unknown), and the values
    // of every Bind that named this statement.
    struct postgres_parse final {
        std::string sql_;
        std::vector<std::uint32_t> parameter_types_;
        std::vector<std::optional<std::string>> parameter_values_;
    };

    // One column of a MariaDB result set: its name, type code, flags,
    // decimals and display length, as a column definition carries them.
    struct maria_column final {
        std::string name_{"v"};
        std::uint8_t type_{0xfd};
        std::uint16_t flags_{0};
        std::uint8_t decimals_{0};
        std::uint32_t length_{1024};
    };

    // How a MariaDB peer answers one COM_QUERY or COM_STMT_EXECUTE: a row set,
    // an OK packet when column_ is empty and no columns_ are given, or an ERR
    // packet when error_ is set. A COM_QUERY gets a one-column text row set of
    // rows_ (null_rows_ marks NULL values); a COM_STMT_EXECUTE gets a binary
    // row set of columns_ and binary_rows_ (encoded values, nullopt for NULL),
    // or of one VAR_STRING column of rows_ when columns_ is empty. status_ is
    // the server status the answer reports (2: autocommit; | 1 inside a
    // transaction). A COM_STMT_PREPARE is answered without a reply, from the
    // columns of the reply its execution will take, unless that reply sets
    // prepare_fails_: then it takes the reply and is refused with error_.
    struct maria_reply final {
        std::string column_{"v"};
        std::vector<std::string> rows_;
        std::vector<bool> null_rows_;
        std::uint64_t affected_rows_{0};
        std::uint64_t insert_id_{0};
        std::uint16_t status_{2};
        std::string error_;
        std::uint16_t code_{1064};
        std::string sql_state_{"42000"};
        // A procedure call: the row set ends flagged with more results, and a
        // final OK packet follows.
        bool procedure_{false};
        bool deferred_{false};
        // Commands read before anything is written: this reply answers the
        // first and the replies after it answer the others, in one write.
        std::size_t batch_{1};
        // After reading the batch, close the connection instead of answering.
        bool drop_{false};
        // After answering, close the connection while the client holds it
        // idle, as a server ending an idle session does.
        bool close_after_{false};
        // Nonzero: held, like deferred_, until release_hold() names it.
        std::size_t hold_{0};
        std::vector<maria_column> columns_;
        std::vector<std::vector<std::optional<std::string>>> binary_rows_;
        bool prepare_fails_{false};
        // Nonzero: the commands this reply answers are not read until
        // release_hold() names it, so the client's writes back up.
        std::size_t read_hold_{0};
        // Non-empty: the query is a LOAD DATA LOCAL INFILE. The peer asks
        // for this file, reads its content and answers OK; a packet out of
        // sequence, as a command sent behind the LOAD would be, is logged as
        // "desync" and closes the connection, as the server does.
        std::string local_infile_;
        // The row set ends with the ERR packet of error_ instead of its EOF,
        // as a statement failing while it produces rows (a DOUBLE out of
        // range) does.
        bool error_after_rows_{false};
        // System variables its OK packet reports as changed, as session
        // tracking does.
        std::vector<std::pair<std::string, std::string>> tracked_;
        // Non-empty: the columns the COM_STMT_PREPARE answer describes in
        // place of columns_, as when a table was altered between the prepare
        // and the execution, or as a UNION of YEAR columns is described.
        std::vector<maria_column> prepare_columns_;
        // Nonzero: send metadata and rows first, then hold the row-set EOF
        // until release_hold() names it. Packet sequence numbers stay intact.
        std::size_t row_end_hold_{0};
        // Like row_end_hold_, but holds only a procedure's trailing OK.
        std::size_t procedure_status_hold_{0};
    };

    explicit backend_startup_peer(protocol selected = protocol::postgresql, bool defer_authentication = true,
        after_startup behavior = after_startup::idle, std::vector<postgres_reply> replies = {},
        std::size_t connections = 1)
        : backend_startup_peer(selected, defer_authentication, behavior, std::move(replies), {}, {}, connections) {}

    // A PostgreSQL peer whose connections, in accept order, each answer their
    // own scripted replies.
    explicit backend_startup_peer(std::vector<std::vector<postgres_reply>> connection_replies)
        : backend_startup_peer(
              protocol::postgresql, false, after_startup::idle, {}, std::move(connection_replies), {}, 0) {}

    // A MariaDB peer that answers its connections' COM_QUERY commands from
    // one script, in order. Unless `session_tracking` is false, it announces
    // session tracking and answers the client's setup of it on each new
    // connection itself, outside the script.
    backend_startup_peer(std::vector<maria_reply> replies, after_startup behavior, std::size_t connections = 1,
        bool session_tracking = true)
        : backend_startup_peer(
              protocol::mariadb, false, behavior, {}, {}, std::move(replies), connections, session_tracking) {}

    ~backend_startup_peer() noexcept(false) {
        asio::post(io_, [this] {
            closing_ = true;
            std::error_code ignored;
            acceptor_.close(ignored);
            for (auto& socket : sockets_) {
                socket.close(ignored);
            }
            gate_.cancel();
            work_.reset();
        });
        thread_.join();
        done_.get();
        for (auto& session : sessions_) {
            session.get();
        }
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void wait_for_startup() {
        if (startup_ready_.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            throw std::runtime_error("PostgreSQL test peer did not receive startup");
        }
        startup_ready_.get();
    }

    // Blocks until a close_after_ reply has closed its connection.
    void wait_for_idle_close() {
        if (idle_close_ready_.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            throw std::runtime_error("backend test peer did not close its idle connection");
        }
        idle_close_ready_.get();
    }

    void authenticate() {
        asio::post(io_, [this] { gate_.cancel(); });
    }

    void release_replies() {
        asio::post(io_, [this] {
            replies_released_ = true;
            gate_.cancel();
        });
    }

    void release_hold(std::size_t hold) {
        asio::post(io_, [this, hold] {
            released_holds_.push_back(hold);
            gate_.cancel();
        });
    }

    // Every Parse message received so far, read on the peer's own thread.
    [[nodiscard]] std::vector<postgres_parse> postgres_parses() {
        return asio::post(io_, asio::use_future([this] { return postgres_parses_; })).get();
    }

    // Every COM_QUERY text received so far, read on the peer's own thread.
    [[nodiscard]] std::vector<std::string> maria_queries() {
        return asio::post(io_, asio::use_future([this] { return maria_queries_; })).get();
    }

    // Every MariaDB command received so far: "query:<sql>", "prepare:<sql>",
    // "execute:<id>:<params>", "reset:<id>", "close:<id>" and, for a LOAD
    // DATA LOCAL INFILE, "infile:<content>", where each parameter reads
    // "i:<n>", "u:<n>", "d:<n>", "t:<n>", "s:<text>" or "null".
    [[nodiscard]] std::vector<std::string> maria_commands() {
        return asio::post(io_, asio::use_future([this] { return maria_commands_; })).get();
    }

    // Startups received so far, read on the peer's own thread.
    [[nodiscard]] std::size_t startups() {
        return asio::post(io_, asio::use_future([this] { return startup_count_; })).get();
    }

    // MariaDB connections that turned on session tracking, read on the
    // peer's own thread.
    [[nodiscard]] std::size_t maria_session_setups() {
        return asio::post(io_, asio::use_future([this] { return maria_session_setups_; })).get();
    }

    void wait_for_disconnects() {
        if (done_.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            throw std::runtime_error("backend test peer did not start its sessions");
        }
        for (const auto& session : sessions_) {
            if (session.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
                throw std::runtime_error("backend test connection was not closed");
            }
        }
    }

#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] db_config config() const {
        return {.driver_ = db_driver::postgresql,
            .host_ = "127.0.0.1",
            .port_ = port(),
            .tls_ = {.mode_ = client_tls_mode::disabled}};
    }
#endif

private:
    backend_startup_peer(protocol selected, bool defer_authentication, after_startup behavior,
        std::vector<postgres_reply> replies, std::vector<std::vector<postgres_reply>> connection_replies,
        std::vector<maria_reply> maria_replies, std::size_t connections, bool maria_session_tracking = true)
        : gate_(io_, asio::steady_timer::time_point::max()),
          defer_authentication_(defer_authentication),
          maria_session_tracking_(maria_session_tracking),
          after_startup_(behavior),
          postgres_replies_(std::move(replies)),
          connection_replies_(std::move(connection_replies)),
          connection_next_(connection_replies_.size(), 0),
          maria_replies_(std::move(maria_replies)),
          startup_ready_(startup_.get_future()),
          idle_close_ready_(idle_close_.get_future()),
          acceptor_(io_, {asio::ip::tcp::v4(), 0}),
          sockets_(make_sockets(io_, std::max(connections, connection_replies_.size()))),
          work_(asio::make_work_guard(io_)),
          done_(asio::co_spawn(io_, serve(selected), asio::use_future)),
          thread_([this] { io_.run(); }) {}

    static std::vector<asio::ip::tcp::socket> make_sockets(asio::io_context& io, std::size_t count) {
        std::vector<asio::ip::tcp::socket> sockets;
        sockets.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            sockets.emplace_back(io);
        }
        return sockets;
    }

    asio::awaitable<void> wait_for_authentication() {
        if (++startup_count_ == 1) {
            startup_.set_value();
        }
        if (defer_authentication_) {
            std::error_code ignored;
            co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        }
    }

    void close_idle_connection(asio::ip::tcp::socket& socket) {
        socket.close();
        if (!idle_closed_) {
            idle_closed_ = true;
            idle_close_.set_value();
        }
    }

    static std::uint32_t integer(const unsigned char* bytes) {
        return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) |
               (std::uint32_t{bytes[2]} << 8) | std::uint32_t{bytes[3]};
    }

    static void append_integer(std::string& bytes, std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            bytes.push_back(static_cast<char>((value >> shift) & 255));
        }
    }

    static void append_short(std::string& bytes, std::uint16_t value) {
        bytes.push_back(static_cast<char>(value >> 8));
        bytes.push_back(static_cast<char>(value & 255));
    }

    static void message(std::string& bytes, char type, std::string_view payload) {
        bytes.push_back(type);
        append_integer(bytes, static_cast<std::uint32_t>(payload.size() + 4));
        bytes.append(payload);
    }

    static void maria_packet(std::string& bytes, std::string_view payload, unsigned char sequence) {
        bytes.push_back(static_cast<char>(payload.size() & 255));
        bytes.push_back(static_cast<char>((payload.size() >> 8) & 255));
        bytes.push_back(static_cast<char>((payload.size() >> 16) & 255));
        bytes.push_back(static_cast<char>(sequence));
        bytes.append(payload);
    }

    asio::awaitable<void> postgres_startup(asio::ip::tcp::socket& socket) {
        for (;;) {
            std::array<unsigned char, 4> length{};
            co_await asio::async_read(socket, asio::buffer(length), asio::use_awaitable);
            const auto size = integer(length.data());
            if (size < 8 || size > 65536) {
                throw std::runtime_error("invalid PostgreSQL startup length");
            }
            std::vector<unsigned char> startup(size - 4);
            co_await asio::async_read(socket, asio::buffer(startup), asio::use_awaitable);
            const auto code = integer(startup.data());
            if (code == 80877103 || code == 80877104) {
                const char no = 'N';
                co_await asio::async_write(socket, asio::buffer(&no, 1), asio::use_awaitable);
                continue;
            }
            break;
        }
        co_await wait_for_authentication();
        if (closing_) {
            co_return;
        }
        std::string response;
        message(response, 'R', std::string(4, '\0'));
        for (const auto& [name, value] : std::array{
                 std::pair{"server_version", "16.6"},
                 std::pair{"server_encoding", "UTF8"},
                 std::pair{"client_encoding", "UTF8"},
                 std::pair{"DateStyle", "ISO, MDY"},
                 std::pair{"integer_datetimes", "on"},
                 std::pair{"standard_conforming_strings", "on"},
                 std::pair{"TimeZone", "UTC"}}) {
            std::string parameter(name);
            parameter.push_back('\0');
            parameter.append(value);
            parameter.push_back('\0');
            message(response, 'S', parameter);
        }
        std::string key;
        append_integer(key, 1);
        append_integer(key, 2);
        message(response, 'K', key);
        message(response, 'Z', "I");
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
    }

    // Reads one flight of extended-protocol messages up to its Sync and
    // returns their types in order; nullopt when the client terminates.
    // Parse, Bind and Close keep the connection's named statements, as a
    // server does. A Parse that `reply` makes fail keeps no statement, and a
    // Bind of that name is recorded as 'b'; a Bind of any other name the
    // session does not hold is a client defect.
    asio::awaitable<std::optional<std::string>> postgres_flight(asio::ip::tcp::socket& socket,
        const postgres_reply* reply, std::map<std::string, std::size_t>& statements) {
        std::string messages;
        for (;;) {
            std::array<unsigned char, 5> header{};
            co_await asio::async_read(socket, asio::buffer(header), asio::use_awaitable);
            const auto size = integer(header.data() + 1);
            if (size < 4 || size > 65536) {
                throw std::runtime_error("invalid PostgreSQL message length");
            }
            std::vector<unsigned char> body(size - 4);
            co_await asio::async_read(socket, asio::buffer(body), asio::use_awaitable);
            auto type = static_cast<char>(header[0]);
            if (type == 'X') {
                co_return std::nullopt;
            }
            if (type == 'P') {
                record_postgres_parse(body, statements, reply != nullptr && reply->parse_fails_);
            } else if (type == 'B') {
                if (!record_postgres_bind(body, statements)) {
                    type = 'b';
                }
            } else if (type == 'C') {
                if (!body.empty() && body[0] == 'S') {
                    statements.erase(text_at(body, 1).first);
                }
            }
            messages.push_back(type);
            if (type == 'S') {
                co_return messages;
            }
        }
    }

    // The NUL-terminated string at `start` and the offset just past it.
    static std::pair<std::string, std::size_t> text_at(const std::vector<unsigned char>& body, std::size_t start) {
        for (auto index = start; index < body.size(); ++index) {
            if (body[index] == 0) {
                return {std::string(reinterpret_cast<const char*>(body.data()) + start, index - start), index + 1};
            }
        }
        throw std::runtime_error("unterminated PostgreSQL string");
    }

    // Parse: statement name, query text, Int16 count, then one Int32 OID each.
    void record_postgres_parse(
        const std::vector<unsigned char>& body, std::map<std::string, std::size_t>& statements, bool fails) {
        auto [name, query_start] = text_at(body, 0);
        auto [sql, offset] = text_at(body, query_start);
        postgres_parse parsed;
        parsed.sql_ = std::move(sql);
        if (offset + 2 > body.size()) {
            throw std::runtime_error("truncated PostgreSQL Parse parameter count");
        }
        const auto count = (std::size_t{body[offset]} << 8) | std::size_t{body[offset + 1]};
        offset += 2;
        if (offset + count * 4 != body.size()) {
            throw std::runtime_error("invalid PostgreSQL Parse parameter types");
        }
        for (std::size_t index = 0; index < count; ++index, offset += 4) {
            parsed.parameter_types_.push_back(integer(body.data() + offset));
        }
        if (!name.empty() && statements.contains(name) && statements.at(name) != failed_parse) {
            throw std::runtime_error("PostgreSQL Parse reuses a prepared statement name");
        }
        postgres_parses_.push_back(std::move(parsed));
        statements[name] = fails ? failed_parse : postgres_parses_.size() - 1;
    }

    // Marks a name whose Parse the script made fail: the server holds none.
    static constexpr std::size_t failed_parse = std::numeric_limits<std::size_t>::max();

    // Bind: portal name, statement name, formats, then the values. False for a
    // name whose Parse failed.
    [[nodiscard]] bool record_postgres_bind(
        const std::vector<unsigned char>& body, const std::map<std::string, std::size_t>& statements) {
        const auto portal_end = text_at(body, 0).second;
        auto [name, offset] = text_at(body, portal_end);
        const auto statement = statements.find(name);
        if (statement == statements.end()) {
            throw std::runtime_error("PostgreSQL Bind names no prepared statement");
        }
        if (statement->second == failed_parse) {
            return false;
        }
        const std::string_view bytes(reinterpret_cast<const char*>(body.data()), body.size());

        const auto read_short = [&]() {
            if (body.size() - offset < 2) {
                throw std::runtime_error("truncated PostgreSQL Bind count");
            }
            const auto value = (std::size_t{body[offset]} << 8) | body[offset + 1];
            offset += 2;
            return value;
        };
        const auto formats = read_short();
        if (formats > (body.size() - offset) / 2) {
            throw std::runtime_error("truncated PostgreSQL Bind formats");
        }
        offset += formats * 2;
        const auto count = read_short();
        auto& values = postgres_parses_[statement->second].parameter_values_;
        for (std::size_t i = 0; i < count; ++i) {
            if (body.size() - offset < 4) {
                throw std::runtime_error("truncated PostgreSQL Bind length");
            }
            const auto length = integer(body.data() + offset);
            offset += 4;
            if (length == std::numeric_limits<std::uint32_t>::max()) {
                values.emplace_back(std::nullopt);
            } else {
                if (length > body.size() - offset) {
                    throw std::runtime_error("truncated PostgreSQL Bind value");
                }
                values.emplace_back(bytes.substr(offset, length));
                offset += length;
            }
        }
        return true;
    }

    static void postgres_error(std::string& response, const postgres_reply& reply) {
        std::string fields;
        for (const auto& [code, value] : std::array{std::pair{'S', std::string_view("ERROR")},
                 std::pair{'C', std::string_view(reply.sql_state_)}, std::pair{'M', std::string_view(reply.error_)},
                 std::pair{'R', std::string_view(reply.routine_)}}) {
            if (value.empty()) {
                continue;
            }
            fields.push_back(code);
            fields.append(value);
            fields.push_back('\0');
        }
        fields.push_back('\0');
        message(response, 'E', fields);
    }

    // Answers each message of a flight in order, with `reply` supplying the
    // statement's outcome. An incomplete answer stops after the first row,
    // before CommandComplete and ReadyForQuery.
    static void postgres_flight_response(
        std::string& response, std::string_view messages, const postgres_reply& reply, bool complete = true) {
        for (const char type : messages) {
            switch (type) {
                case 'C':
                    message(response, '3', {});
                    break;
                case 'P':
                    if (reply.parse_fails_) {
                        // The server skips the rest of the flight up to Sync.
                        postgres_error(response, reply);
                        message(response, 'Z', std::string_view(&reply.status_, 1));
                        return;
                    }
                    message(response, '1', {});
                    break;
                case 'b': {
                    const postgres_reply missing{.error_ = "prepared statement does not exist",
                        .sql_state_ = "26000",
                        .routine_ = "FetchPreparedStatement"};
                    postgres_error(response, missing);
                    message(response, 'Z', std::string_view(&reply.status_, 1));
                    return;
                }
                case 'B':
                    if (!reply.error_.empty() && !reply.parse_fails_) {
                        // The server skips the rest of the flight up to Sync.
                        postgres_error(response, reply);
                        message(response, 'Z', std::string_view(&reply.status_, 1));
                        return;
                    }
                    message(response, '2', {});
                    break;
                case 'D':
                    if (reply.copy_ != postgres_copy_direction::none) {
                        break;
                    }
                    if (reply.column_.empty() && reply.columns_.empty()) {
                        message(response, 'n', {});
                        break;
                    }
                    postgres_description(response, reply);
                    break;
                case 'E':
                    if (reply.copy_ != postgres_copy_direction::none) {
                        // Text format, one column in text format.
                        std::string copy;
                        copy.push_back('\0');
                        append_short(copy, 1);
                        append_short(copy, 0);
                        message(response, reply.copy_ == postgres_copy_direction::input ? 'G' : 'H', copy);
                        return;
                    }
                    for (std::size_t index = 0; index < reply.rows_.size(); ++index) {
                        const bool null = index < reply.null_rows_.size() && reply.null_rows_[index];
                        const auto& value = reply.rows_[index];
                        std::string row;
                        append_short(row, 1);
                        append_integer(row, null ? std::numeric_limits<std::uint32_t>::max()
                                                 : static_cast<std::uint32_t>(value.size()));
                        if (!null) {
                            row.append(value);
                        }
                        message(response, 'D', row);
                        if (!complete) {
                            return;
                        }
                    }
                    for (const auto& values : reply.values_) {
                        std::string row;
                        append_short(row, static_cast<std::uint16_t>(values.size()));
                        for (const auto& value : values) {
                            append_integer(row, static_cast<std::uint32_t>(value.size()));
                            row.append(value);
                        }
                        message(response, 'D', row);
                        if (!complete) {
                            return;
                        }
                    }
                    if (!complete) {
                        return;
                    }
                    {
                        std::string command(reply.command_);
                        command.push_back('\0');
                        message(response, 'C', command);
                    }
                    break;
                case 'S':
                    message(response, 'Z', std::string_view(&reply.status_, 1));
                    break;
                default:
                    break;
            }
        }
    }

    static void postgres_description(std::string& response, const postgres_reply& reply) {
        std::string description;
        const auto columns = reply.columns_.empty() ? std::span<const std::string>(&reply.column_, 1) : std::span<const std::string>(reply.columns_);
        append_short(description, static_cast<std::uint16_t>(columns.size()));
        for (const auto& column : columns) {
            description.append(column);
            description.push_back('\0');
            append_integer(description, 0);
            append_short(description, 0);
            append_integer(description, reply.type_);
            append_short(description, 0xffff);
            append_integer(description, 0xffffffff);
            append_short(description, 0);
        }
        message(response, 'T', description);
    }

    // CommandComplete and ReadyForQuery never follow the partial row set.
    asio::awaitable<void> postgres_drop_mid_result(asio::ip::tcp::socket& socket) {
        std::map<std::string, std::size_t> statements;
        const auto messages = co_await postgres_flight(socket, nullptr, statements);
        if (!messages) {
            co_return;
        }
        std::string response;
        postgres_flight_response(response, *messages, {.rows_ = {"x"}}, false);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
    }

    template <typename reply_type>
    [[nodiscard]] bool held(const reply_type& reply) const {
        return (reply.deferred_ && !replies_released_) ||
               (reply.hold_ != 0 && std::ranges::find(released_holds_, reply.hold_) == released_holds_.end());
    }

    // Answers `replies` from `next` in order, each batch of flights in one
    // write. Returns true when a dropping reply closed the connection.
    asio::awaitable<bool> postgres_script(asio::ip::tcp::socket& socket, std::vector<postgres_reply>& replies,
        std::size_t& next, std::map<std::string, std::size_t>& statements) {
        while (next < replies.size()) {
            const auto& reply = replies[next];
            std::vector<std::string> flights;
            while (flights.size() < std::max<std::size_t>(reply.batch_, 1)) {
                const auto answer = next + flights.size();
                auto messages =
                    co_await postgres_flight(socket, answer < replies.size() ? &replies[answer] : nullptr, statements);
                if (!messages) {
                    co_return false;
                }
                flights.push_back(std::move(*messages));
            }
            std::size_t written_prefix = 0;
            if (reply.first_row_before_hold_) {
                if (flights.size() != 1 || (reply.rows_.empty() && reply.values_.empty())) {
                    throw std::invalid_argument("a split PostgreSQL reply requires one flight and a row");
                }
                std::string first;
                postgres_flight_response(first, flights.front(), reply, false);
                co_await asio::async_write(socket, asio::buffer(first), asio::use_awaitable);
                written_prefix = first.size();
            }
            while (held(reply) && !closing_) {
                std::error_code ignored;
                co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
            }
            if (closing_) {
                co_return false;
            }
            if (reply.drop_) {
                ++next;
                socket.close();
                co_return true;
            }
            std::string response;
            for (const auto& messages : flights) {
                if (next == replies.size()) {
                    break;
                }
                postgres_flight_response(response, messages, replies[next++]);
            }
            co_await asio::async_write(socket,
                asio::buffer(response.data() + written_prefix, response.size() - written_prefix), asio::use_awaitable);
            if (reply.close_after_) {
                close_idle_connection(socket);
                co_return true;
            }
        }
        co_return false;
    }

    asio::awaitable<void> maria_packet(asio::ip::tcp::socket& socket, std::string_view payload, unsigned char sequence) {
        std::string bytes;
        maria_packet(bytes, payload, sequence);
        co_await asio::async_write(socket, asio::buffer(bytes), asio::use_awaitable);
    }

    asio::awaitable<void> maria_startup(asio::ip::tcp::socket& socket) {
        // Protocol 10 greeting, protocol-41 secure connection and native auth.
        std::string greeting(1, '\x0a');
        greeting.append("8.0.0-test", 11);
        greeting.append("\x01\0\0\0", 4);
        greeting.append("12345678\0", 9);
        greeting.append("\x05\xa2", 2);
        greeting.push_back('\x2d');
        greeting.append("\x02\0", 2);
        // Upper capabilities: plugin auth and multiple results, and session
        // tracking unless the test turned it off.
        greeting.push_back(maria_session_tracking_ ? '\x8a' : '\x0a');
        greeting.push_back('\0');
        greeting.push_back('\x15');
        greeting.append(10, '\0');
        greeting.append("abcdefghijkl\0", 13);
        greeting.append("mysql_native_password", 22);
        co_await maria_packet(socket, greeting, 0);
        std::array<unsigned char, 4> header{};
        co_await asio::async_read(socket, asio::buffer(header), asio::use_awaitable);
        const auto size = std::size_t{header[0]} | (std::size_t{header[1]} << 8) |
                          (std::size_t{header[2]} << 16);
        if (size > 65536) {
            throw std::runtime_error("invalid MariaDB handshake length");
        }
        std::vector<unsigned char> handshake(size);
        co_await asio::async_read(socket, asio::buffer(handshake), asio::use_awaitable);
        co_await wait_for_authentication();
        if (closing_) {
            co_return;
        }
        co_await maria_packet(socket, std::string_view("\0\0\0\x02\0\0\0", 7),
            static_cast<unsigned char>(header[3] + 1));
        if (!maria_session_tracking_) {
            co_return;
        }
        // The client turns on tracking of the variables its statements depend
        // on, by name, before anything else; the peer reports the change as
        // the server does.
        std::array<unsigned char, 4> setup_header{};
        co_await asio::async_read(socket, asio::buffer(setup_header), asio::use_awaitable);
        const auto setup_size =
            std::size_t{setup_header[0]} | (std::size_t{setup_header[1]} << 8) | (std::size_t{setup_header[2]} << 16);
        std::string setup(setup_size, '\0');
        co_await asio::async_read(socket, asio::buffer(setup), asio::use_awaitable);
        constexpr std::string_view setup_start = "\x03SET SESSION session_track_system_variables = '";
        constexpr std::string_view setup_end = "', session_track_schema = ON";
        if (!setup.starts_with(setup_start) || !setup.ends_with(setup_end) ||
            setup.find("sql_mode,") == std::string::npos || setup.find('*') != std::string::npos) {
            throw std::runtime_error("MariaDB client did not turn on session tracking first");
        }
        ++maria_session_setups_;
        const auto tracked =
            setup.substr(setup_start.size(), setup.size() - setup_start.size() - setup_end.size());
        co_await maria_packet(
            socket, maria_ok(0, 0, 2, {{"session_track_schema", "ON"}, {"session_track_system_variables", tracked}}), 1);
    }

    // Column count, one VARCHAR column definition and its EOF, and one row
    // follow COM_QUERY; the closing EOF never does.
    asio::awaitable<void> maria_drop_mid_result(asio::ip::tcp::socket& socket) {
        std::array<unsigned char, 4> header{};
        co_await asio::async_read(socket, asio::buffer(header), asio::use_awaitable);
        const auto size = std::size_t{header[0]} | (std::size_t{header[1]} << 8) | (std::size_t{header[2]} << 16);
        if (size > 65536) {
            throw std::runtime_error("invalid MariaDB command length");
        }
        std::vector<unsigned char> command(size);
        co_await asio::async_read(socket, asio::buffer(command), asio::use_awaitable);
        auto sequence = static_cast<unsigned char>(header[3] + 1);
        std::string response;
        maria_packet(response, std::string_view("\x01", 1), sequence++);
        std::string column;
        for (const std::string_view part : {"def", "", "", "", "v", ""}) {
            column.push_back(static_cast<char>(part.size()));
            column.append(part);
        }
        // Fixed fields: charset utf8mb4, length 1024, VAR_STRING, no flags.
        column.append("\x0c\x2d\x00\x00\x04\x00\x00\xfd\x00\x00\x00\x00\x00", 13);
        maria_packet(response, column, sequence++);
        maria_packet(response, std::string_view("\xfe\0\0\x02\0", 5), sequence++);
        maria_packet(response, std::string_view("\x01x", 2), sequence++);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
    }

    static void maria_length(std::string& bytes, std::uint64_t value) {
        if (value < 251) {
            bytes.push_back(static_cast<char>(value));
            return;
        }
        const auto [marker, width] = value < 65536 ? std::pair{'\xfc', 2} : std::pair{'\xfe', 8};
        bytes.push_back(marker);
        for (int index = 0; index < width; ++index) {
            bytes.push_back(static_cast<char>((value >> (8 * index)) & 255));
        }
    }

    static void maria_status(std::string& bytes, std::uint16_t status) {
        bytes.push_back(static_cast<char>(status & 255));
        bytes.push_back(static_cast<char>(status >> 8));
    }

    // An OK packet; with `tracked` system variables it reports them as
    // changed (SERVER_SESSION_STATE_CHANGED), as session tracking does.
    static std::string maria_ok(std::uint64_t affected, std::uint64_t insert_id, std::uint16_t status,
        const std::vector<std::pair<std::string, std::string>>& tracked) {
        std::string packet(1, '\0');
        maria_length(packet, affected);
        maria_length(packet, insert_id);
        maria_status(packet, static_cast<std::uint16_t>(tracked.empty() ? status : status | 0x4000));
        packet.append(2, '\0');
        if (!tracked.empty()) {
            std::string changes;
            for (const auto& [name, value] : tracked) {
                std::string item;
                maria_length(item, name.size());
                item.append(name);
                maria_length(item, value.size());
                item.append(value);
                // SESSION_TRACK_SYSTEM_VARIABLES, then the item.
                changes.push_back('\0');
                maria_length(changes, item.size());
                changes.append(item);
            }
            // No info text, then the session state changes.
            maria_length(packet, 0);
            maria_length(packet, changes.size());
            packet.append(changes);
        }
        return packet;
    }

    // One command the client sent.
    struct maria_request final {
        char type_{0};
        std::string text_;
    };

    // A prepared statement the peer handed out: its parameter count and the
    // parameter types its last execution bound.
    struct maria_statement final {
        std::size_t params_{0};
        std::vector<std::uint16_t> types_;
    };

    // Reads one command and logs it; nullopt when the client quits.
    asio::awaitable<std::optional<maria_request>> maria_command(asio::ip::tcp::socket& socket) {
        std::array<unsigned char, 4> header{};
        co_await asio::async_read(socket, asio::buffer(header), asio::use_awaitable);
        const auto size = std::size_t{header[0]} | (std::size_t{header[1]} << 8) | (std::size_t{header[2]} << 16);
        if (size == 0 || header[3] != 0) {
            throw std::runtime_error("invalid MariaDB command packet");
        }
        std::string command(size, '\0');
        co_await asio::async_read(socket, asio::buffer(command), asio::use_awaitable);
        maria_request request{command[0], command.substr(1)};
        switch (request.type_) {
            case '\x01':
                co_return std::nullopt;
            case '\x03':
                maria_queries_.push_back(request.text_);
                maria_commands_.push_back("query:" + request.text_);
                break;
            case '\x16':
                maria_commands_.push_back("prepare:" + request.text_);
                break;
            case '\x17':
                maria_commands_.push_back(maria_execution(request.text_));
                break;
            case '\x19':
                maria_commands_.push_back(std::string("close:") + std::to_string(maria_integer(request.text_, 0, 4)).c_str());
                break;
            case '\x1a':
                maria_commands_.push_back(std::string("reset:") + std::to_string(maria_integer(request.text_, 0, 4)).c_str());
                break;
            default:
                throw std::runtime_error("unexpected MariaDB command");
        }
        co_return request;
    }

    static std::uint64_t maria_integer(std::string_view bytes, std::size_t offset, std::size_t width) {
        if (offset + width > bytes.size()) {
            throw std::runtime_error("truncated MariaDB integer");
        }
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < width; ++index) {
            value |= std::uint64_t{static_cast<unsigned char>(bytes[offset + index])} << (8 * index);
        }
        return value;
    }

    // COM_STMT_EXECUTE: statement id, flags, iteration count, then a NULL
    // bitmap, whether types follow, the types, and each non-NULL value.
    std::string maria_execution(std::string_view payload) {
        const auto id = static_cast<std::uint32_t>(maria_integer(payload, 0, 4));
        auto found = maria_statements_.find(id);
        if (found == maria_statements_.end()) {
            throw std::runtime_error("MariaDB execute names no prepared statement");
        }
        auto& statement = found->second;
        std::string logged = std::string("execute:") + std::to_string(id).c_str() + ":";
        std::size_t offset = 9;
        if (statement.params_ == 0) {
            return logged;
        }
        const auto bitmap = offset;
        offset += (statement.params_ + 7) / 8;
        if (maria_integer(payload, offset++, 1) != 0) {
            statement.types_.clear();
            for (std::size_t index = 0; index < statement.params_; ++index, offset += 2) {
                statement.types_.push_back(static_cast<std::uint16_t>(maria_integer(payload, offset, 2)));
            }
        }
        for (std::size_t index = 0; index < statement.params_; ++index) {
            if (index != 0) {
                logged.push_back(',');
            }
            if ((maria_integer(payload, bitmap + index / 8, 1) & (std::uint64_t{1} << (index % 8))) != 0) {
                logged.append("null");
                continue;
            }
            const auto type = statement.types_.at(index);
            switch (type & 0xff) {
                case 0x08: {
                    const auto value = maria_integer(payload, offset, 8);
                    offset += 8;
                    logged.append((type & 0x8000) != 0 ? std::string("u:") + std::to_string(value).c_str()
                                                       : std::string("i:") + std::to_string(static_cast<std::int64_t>(value)).c_str());
                    break;
                }
                case 0x05: {
                    const auto bits = maria_integer(payload, offset, 8);
                    offset += 8;
                    double value = 0;
                    std::memcpy(&value, &bits, sizeof(value));
                    std::array<char, 32> text{};
                    const auto converted = std::to_chars(text.data(), text.data() + text.size(), value);
                    logged.append("d:").append(text.data(), converted.ptr);
                    break;
                }
                case 0x01:
                    logged.append(std::string("t:") + std::to_string(maria_integer(payload, offset++, 1)).c_str());
                    break;
                default: {
                    auto length = maria_integer(payload, offset++, 1);
                    if (length == 0xfc) {
                        length = maria_integer(payload, offset, 2);
                        offset += 2;
                    }
                    logged.append("s:").append(payload.substr(offset, static_cast<std::size_t>(length)));
                    offset += static_cast<std::size_t>(length);
                    break;
                }
            }
        }
        return logged;
    }

    static void maria_column_definition(
        std::string& response, const maria_column& definition, unsigned char& sequence) {
        std::string column;
        for (const std::string_view part : {std::string_view("def"), std::string_view(), std::string_view(),
                 std::string_view(), std::string_view(definition.name_), std::string_view()}) {
            column.push_back(static_cast<char>(part.size()));
            column.append(part);
        }
        column.push_back('\x0c');
        // utf8mb4 for text, binary otherwise.
        column.append(definition.type_ >= 0xf5 ? std::string_view("\x2d\0", 2) : std::string_view("\x3f\0", 2));
        for (int shift = 0; shift < 32; shift += 8) {
            column.push_back(static_cast<char>((definition.length_ >> shift) & 255));
        }
        column.push_back(static_cast<char>(definition.type_));
        column.push_back(static_cast<char>(definition.flags_ & 255));
        column.push_back(static_cast<char>(definition.flags_ >> 8));
        column.push_back(static_cast<char>(definition.decimals_));
        column.append(2, '\0');
        maria_packet(response, column, sequence++);
    }

    [[nodiscard]] static std::vector<maria_column> maria_columns(const maria_reply& reply) {
        if (!reply.columns_.empty()) {
            return reply.columns_;
        }
        if (reply.column_.empty()) {
            return {};
        }
        return {maria_column{.name_ = reply.column_}};
    }

    // COM_STMT_PREPARE's answer: the statement id, its column and parameter
    // counts, and their definitions.
    void maria_prepared(std::string& response, std::string_view sql, const maria_reply* executed) {
        const auto id = ++maria_statement_ids_;
        const auto params = static_cast<std::size_t>(std::ranges::count(sql, '?'));
        maria_statements_[id] = maria_statement{params, {}};
        const auto columns = executed == nullptr                  ? std::vector<maria_column>{}
                             : executed->prepare_columns_.empty() ? maria_columns(*executed)
                                                                  : executed->prepare_columns_;
        unsigned char sequence = 1;
        std::string packet(1, '\0');
        for (int shift = 0; shift < 32; shift += 8) {
            packet.push_back(static_cast<char>((id >> shift) & 255));
        }
        packet.push_back(static_cast<char>(columns.size() & 255));
        packet.push_back(static_cast<char>(columns.size() >> 8));
        packet.push_back(static_cast<char>(params & 255));
        packet.push_back(static_cast<char>(params >> 8));
        packet.append(3, '\0');
        maria_packet(response, packet, sequence++);
        std::string eof("\xfe\0\0\x02\0", 5);
        if (params != 0) {
            for (std::size_t index = 0; index < params; ++index) {
                maria_column_definition(response, {.name_ = "?"}, sequence);
            }
            maria_packet(response, eof, sequence++);
        }
        if (!columns.empty()) {
            for (const auto& column : columns) {
                maria_column_definition(response, column, sequence);
            }
            maria_packet(response, eof, sequence++);
        }
    }

    // The packets answering one command, sequence numbers from 1: text rows
    // for a COM_QUERY, binary rows for a COM_STMT_EXECUTE.
    static void maria_response(std::string& response, const maria_reply& reply, bool binary,
        std::vector<std::pair<std::size_t, std::size_t>>& holds) {
        unsigned char sequence = 1;
        const auto ok = [&](std::uint64_t affected, std::uint64_t insert_id, std::uint16_t status,
                            const std::vector<std::pair<std::string, std::string>>& tracked) {
            maria_packet(response, maria_ok(affected, insert_id, status, tracked), sequence++);
        };
        const auto error = [&] {
            std::string packet(1, '\xff');
            packet.push_back(static_cast<char>(reply.code_ & 255));
            packet.push_back(static_cast<char>(reply.code_ >> 8));
            packet.push_back('#');
            packet.append(reply.sql_state_);
            packet.append(reply.error_);
            maria_packet(response, packet, sequence++);
        };
        if (!reply.error_.empty() && !reply.error_after_rows_) {
            error();
            return;
        }
        const auto columns = binary ? maria_columns(reply) : maria_columns({.column_ = reply.column_});
        if (columns.empty()) {
            ok(reply.affected_rows_, reply.insert_id_, reply.status_, reply.tracked_);
            return;
        }
        std::string count;
        maria_length(count, columns.size());
        maria_packet(response, count, sequence++);
        for (const auto& column : columns) {
            maria_column_definition(response, column, sequence);
        }
        std::string eof("\xfe\0\0", 3);
        maria_status(eof, reply.status_);
        maria_packet(response, eof, sequence++);
        const auto put_text = [](std::string& row, std::string_view value) {
            maria_length(row, value.size());
            row.append(value);
        };
        if (binary && !reply.columns_.empty()) {
            for (const auto& values : reply.binary_rows_) {
                std::string row(1, '\0');
                const auto bitmap = row.size();
                row.append((columns.size() + 9) / 8, '\0');
                for (std::size_t index = 0; index < values.size(); ++index) {
                    if (!values[index].has_value()) {
                        row[bitmap + (index + 2) / 8] =
                            static_cast<char>(row[bitmap + (index + 2) / 8] | (1 << ((index + 2) % 8)));
                        continue;
                    }
                    row.append(*values[index]);
                }
                maria_packet(response, row, sequence++);
            }
        } else {
            for (std::size_t index = 0; index < reply.rows_.size(); ++index) {
                const bool null = index < reply.null_rows_.size() && reply.null_rows_[index];
                std::string row;
                if (binary) {
                    row.push_back('\0');
                    row.push_back(null ? '\x04' : '\0');
                    if (!null) {
                        put_text(row, reply.rows_[index]);
                    }
                } else if (null) {
                    row.push_back('\xfb');
                } else {
                    put_text(row, reply.rows_[index]);
                }
                maria_packet(response, row, sequence++);
            }
        }
        if (reply.error_after_rows_) {
            error();
            return;
        }
        if (reply.row_end_hold_ != 0) {
            holds.emplace_back(response.size(), reply.row_end_hold_);
        }
        // Server status bit 8: more results follow.
        eof = std::string("\xfe\0\0", 3);
        maria_status(eof, static_cast<std::uint16_t>(reply.procedure_ ? reply.status_ | 8 : reply.status_));
        maria_packet(response, eof, sequence++);
        if (reply.procedure_) {
            if (reply.procedure_status_hold_ != 0) {
                holds.emplace_back(response.size(), reply.procedure_status_hold_);
            }
            ok(0, 0, reply.status_, {});
        }
    }

    // Answers a LOAD DATA LOCAL INFILE: asks for reply.local_infile_, reads the
    // file's packets up to the empty one that ends it, and answers OK. Returns
    // true when a packet out of sequence closed the connection.
    asio::awaitable<bool> maria_local_infile(asio::ip::tcp::socket& socket, const maria_reply& reply) {
        unsigned char sequence = 1;
        co_await maria_packet(socket, "\xfb" + reply.local_infile_, sequence++);
        std::string content;
        for (;;) {
            std::array<unsigned char, 4> header{};
            co_await asio::async_read(socket, asio::buffer(header), asio::use_awaitable);
            const auto size = std::size_t{header[0]} | (std::size_t{header[1]} << 8) | (std::size_t{header[2]} << 16);
            std::string payload(size, '\0');
            if (size != 0) {
                co_await asio::async_read(socket, asio::buffer(payload), asio::use_awaitable);
            }
            if (header[3] != sequence) {
                maria_commands_.push_back("desync");
                socket.close();
                co_return true;
            }
            ++sequence;
            if (size == 0) {
                break;
            }
            content += payload;
        }
        maria_commands_.push_back("infile:" + content);
        std::string ok(1, '\0');
        maria_length(ok, reply.affected_rows_);
        maria_length(ok, 0);
        maria_status(ok, reply.status_);
        ok.append(2, '\0');
        co_await maria_packet(socket, ok, sequence);
        co_return false;
    }

    // Answers the script from next_maria_ in order, each batch of commands in
    // one write. A prepare is answered at once, and a close needs no answer.
    // Returns true when a dropping reply closed the connection.
    asio::awaitable<bool> maria_script(asio::ip::tcp::socket& socket) {
        while (next_maria_ < maria_replies_.size()) {
            const auto& reply = maria_replies_[next_maria_];
            while (reply.read_hold_ != 0 &&
                   std::ranges::find(released_holds_, reply.read_hold_) == released_holds_.end() && !closing_) {
                std::error_code ignored;
                co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
            }
            if (closing_) {
                co_return false;
            }
            std::vector<char> answered;
            while (answered.size() < std::max<std::size_t>(reply.batch_, 1)) {
                const auto request = co_await maria_command(socket);
                if (!request) {
                    co_return false;
                }
                if (request->type_ == '\x19') {
                    continue;
                }
                if (request->type_ == '\x1a') {
                    // A statement reset is answered at once.
                    const auto ok = maria_ok(0, 0, 2, {});
                    std::string packet;
                    maria_packet(packet, ok, 1);
                    co_await asio::async_write(socket, asio::buffer(packet), asio::use_awaitable);
                    continue;
                }
                const auto taken = next_maria_ + answered.size();
                const auto* next = taken < maria_replies_.size() ? &maria_replies_[taken] : nullptr;
                if (request->type_ == '\x16' && (next == nullptr || !next->prepare_fails_)) {
                    std::string prepared;
                    maria_prepared(prepared, request->text_, next);
                    co_await asio::async_write(socket, asio::buffer(prepared), asio::use_awaitable);
                    continue;
                }
                answered.push_back(request->type_);
            }
            while (held(reply) && !closing_) {
                std::error_code ignored;
                co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
            }
            if (closing_) {
                co_return false;
            }
            if (reply.drop_) {
                ++next_maria_;
                socket.close();
                co_return true;
            }
            if (!reply.local_infile_.empty()) {
                ++next_maria_;
                if (co_await maria_local_infile(socket, reply)) {
                    co_return true;
                }
                continue;
            }
            std::string response;
            std::vector<std::pair<std::size_t, std::size_t>> holds;
            for (const char type : answered) {
                if (next_maria_ == maria_replies_.size()) {
                    break;
                }
                maria_response(response, maria_replies_[next_maria_++], type == '\x17', holds);
            }
            std::size_t sent = 0;
            for (const auto& [offset, hold] : holds) {
                if (offset != sent) {
                    co_await asio::async_write(socket, asio::buffer(response.data() + sent, offset - sent), asio::use_awaitable);
                    sent = offset;
                }
                while (std::ranges::find(released_holds_, hold) == released_holds_.end() && !closing_) {
                    std::error_code ignored;
                    co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
                }
                if (closing_) {
                    co_return false;
                }
            }
            co_await asio::async_write(socket, asio::buffer(response.data() + sent, response.size() - sent), asio::use_awaitable);
            if (reply.close_after_) {
                close_idle_connection(socket);
                co_return true;
            }
        }
        co_return false;
    }

    asio::awaitable<void> serve(protocol selected) {
        for (auto& socket : sockets_) {
            sessions_.push_back(asio::co_spawn(io_, serve_session(selected, socket), asio::use_future));
        }
        co_return;
    }

    asio::awaitable<void> serve_session(protocol selected, asio::ip::tcp::socket& socket) {
        for (;;) {
            try {
                co_await serve_connection(selected, socket);
            } catch (const std::system_error& error) {
                // EOF/reset/connection abort/broken pipe are the client's public close path
                // (a reply written after the client closed). Only cancellation
                // caused by this peer's own teardown may suppress an aborted wait.
                if (error.code() != asio::error::eof &&
                    error.code() != asio::error::connection_reset &&
                    error.code() != asio::error::connection_aborted &&
                    error.code() != asio::error::broken_pipe &&
                    !(closing_ && (error.code() == asio::error::operation_aborted ||
                                      error.code() == asio::error::bad_descriptor))) {
                    throw;
                }
            }
            const bool script_done = selected == protocol::mariadb ? next_maria_ >= maria_replies_.size()
                                                                   : next_reply_ >= postgres_replies_.size();
            if (closing_ || after_startup_ != after_startup::reaccept || script_done) {
                co_return;
            }
            socket = asio::ip::tcp::socket(io_);
        }
    }

    asio::awaitable<void> serve_connection(protocol selected, asio::ip::tcp::socket& socket) {
        co_await acceptor_.async_accept(socket, asio::use_awaitable);
        const auto connection = accepted_++;
        // Named statements belong to one server session.
        std::map<std::string, std::size_t> statements;
        if (selected == protocol::postgresql) {
            co_await postgres_startup(socket);
        } else if (selected == protocol::mariadb) {
            co_await maria_startup(socket);
        }
        if (closing_) {
            co_return;
        }
        if (after_startup_ == after_startup::drop_mid_result) {
            if (selected == protocol::postgresql) {
                co_await postgres_drop_mid_result(socket);
            } else if (selected == protocol::mariadb) {
                co_await maria_drop_mid_result(socket);
            }
            socket.close();
            co_return;
        }
        if (selected == protocol::postgresql) {
            // A connection with a script of its own serves that one.
            const bool own = connection < connection_replies_.size();
            auto& replies = own ? connection_replies_[connection] : postgres_replies_;
            auto& next = own ? connection_next_[connection] : next_reply_;
            if (!replies.empty() && co_await postgres_script(socket, replies, next, statements)) {
                co_return;
            }
        }
        if (selected == protocol::mariadb && !maria_replies_.empty() && co_await maria_script(socket)) {
            co_return;
        }
        std::array<char, 64> buffer{};
        for (;;) {
            co_await socket.async_read_some(asio::buffer(buffer), asio::use_awaitable);
        }
    }

    asio::io_context io_;
    asio::steady_timer gate_;
    bool defer_authentication_;
    // The MariaDB peer announces session tracking and answers its setup.
    bool maria_session_tracking_;
    after_startup after_startup_;
    std::vector<postgres_reply> postgres_replies_;
    // Scripts of the connections that have their own, in accept order.
    std::vector<std::vector<postgres_reply>> connection_replies_;
    std::vector<std::size_t> connection_next_;
    std::vector<postgres_parse> postgres_parses_;
    std::vector<maria_reply> maria_replies_;
    std::vector<std::string> maria_queries_;
    std::vector<std::string> maria_commands_;
    std::map<std::uint32_t, maria_statement> maria_statements_;
    std::uint32_t maria_statement_ids_{0};
    std::size_t next_maria_{0};
    std::vector<std::size_t> released_holds_;
    std::size_t accepted_{0};
    std::size_t next_reply_{0};
    std::size_t startup_count_{0};
    std::size_t maria_session_setups_{0};
    bool replies_released_{false};
    bool closing_{false};
    std::promise<void> startup_;
    std::future<void> startup_ready_;
    std::promise<void> idle_close_;
    std::future<void> idle_close_ready_;
    bool idle_closed_{false};
    asio::ip::tcp::acceptor acceptor_;
    std::vector<asio::ip::tcp::socket> sockets_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::vector<std::future<void>> sessions_;
    std::future<void> done_;
    std::thread thread_;
};

inline task<void> observe_backend_completion(task<void> operation, bool& completed) {
    try {
        co_await std::move(operation);
    } catch (...) {
        completed = true;
        throw;
    }
    completed = true;
}

inline void drive_backend_task(event_loop_attachment& attachment, task<void> operation) {
    bool done = false;
    auto loop = attachment.loop();
    auto completed = loop.start(observe_backend_completion(std::move(operation), done));
    auto& io = loop.io_context();
    io.restart();
    // The observer runs before the root completion handler is retired. Keep the
    // owner running until root.wait() publishes completion, not merely until
    // the observer sets its flag.
    std::jthread runner([&attachment] { attachment.run(); });
    completed.wait();
    io.stop();
    runner.join();
    std::move(completed).get();
}

inline void retire_backend_attachment(event_loop_attachment& attachment) {
    auto loop = attachment.loop();
    if (!loop.worker().valid()) {
        return;
    }
    auto& io = loop.io_context();
    attachment.stop();
    io.restart();
    attachment.run();
}

// Startup and public client operations share the attached owner. Shutdown is
// awaited before propagating a callback failure.
template <typename client_type, typename callback_type>
task<void> exercise_backend_client(client_type& client, callback_type& callback) {
    std::exception_ptr failure;
    try {
        co_await client.connect();
        co_await callback(client);
    } catch (...) {
        failure = std::current_exception();
    }
    co_await client.shutdown();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

template <typename client_type, typename callback_type>
void drive_backend_client(
    event_loop_attachment& attachment, client_type& client, callback_type&& callback) {
    try {
        drive_backend_task(attachment, exercise_backend_client(client, callback));
    } catch (...) {
        retire_backend_attachment(attachment);
        throw;
    }
    retire_backend_attachment(attachment);
}

#ifdef RUVIA_ENABLE_DATABASE
template <typename callback_type>
void with_connected_db_client(event_loop_attachment& attachment, db_config config,
    backend_startup_peer::after_startup behavior, callback_type&& callback) {
    backend_startup_peer peer(config.driver_ == db_driver::postgresql ? backend_startup_peer::protocol::postgresql
                                                                      : backend_startup_peer::protocol::mariadb,
        false, behavior, {}, config.pool_size_per_worker_);
    config.host_ = "127.0.0.1";
    config.port_ = peer.port();
    config.username_ = "test";
    config.password_.clear();
    config.database_.clear();
    config.tls_ = {.mode_ = client_tls_mode::disabled};
    db_client client(attachment.loop(), config);
    drive_backend_client(attachment, client, std::forward<callback_type>(callback));
}

template <typename callback_type>
void with_connected_db_client(event_loop_attachment& attachment, db_config config, callback_type&& callback) {
    with_connected_db_client(attachment, std::move(config), backend_startup_peer::after_startup::idle,
        std::forward<callback_type>(callback));
}
#endif

#ifdef RUVIA_ENABLE_REDIS
template <typename callback_type>
void with_connected_redis_client(
    event_loop_attachment& attachment, redis_config config, callback_type&& callback) {
    backend_startup_peer peer(backend_startup_peer::protocol::redis, false);
    config.host_ = "127.0.0.1";
    config.port_ = peer.port();
    config.pool_size_per_worker_ = 1;
    config.tls_ = {.mode_ = client_tls_mode::disabled};
    redis_client client(attachment.loop(), config);
    drive_backend_client(attachment, client, std::forward<callback_type>(callback));
}
#endif

}  // namespace ruvia::test

// Callers shared by the pipelining tests of every driver.
namespace pipeline_test {

using peer_type = ruvia::test::backend_startup_peer;

// What one concurrent caller observed. Failures are recorded rather than
// thrown so every caller in a scope finishes on its own.
struct outcome final {
    std::string value_{};
    std::uint64_t affected_rows_{0};
    std::optional<std::uint64_t> insert_id_;
    std::optional<ruvia::db_error::code_type> error_;
    std::string sql_state_{};
    // The statement was refused for its shape (rows where none were wanted,
    // or none where rows were).
    bool refused_{false};
};

inline void record_failure(const ruvia::db_error& error, outcome& result) {
    result.error_ = error.code();
    result.sql_state_ = error.sql_state().value_or("");
}

inline ruvia::task<void> query(ruvia::db_session handle, std::string_view sql, outcome& result) {
    try {
        const ruvia::db_rows rows = co_await handle.query(sql);
        result.value_ = std::string(rows.front()["v"].as<std::string_view>().value_or(""));
    } catch (const ruvia::db_error& error) {
        record_failure(error, result);
    } catch (const std::invalid_argument&) {
        result.refused_ = true;
    }
}

inline ruvia::task<void> query_id(ruvia::db_session handle, std::string_view sql, std::int64_t id, outcome& result) {
    try {
        const ruvia::db_rows rows = co_await handle.query(sql, id);
        result.value_ = std::string(rows.front()["v"].as<std::string_view>().value_or(""));
    } catch (const ruvia::db_error& error) {
        record_failure(error, result);
    }
}

inline ruvia::task<void> execute(ruvia::db_session handle, std::string_view sql, outcome& result) {
    try {
        const auto executed = co_await handle.execute(sql);
        result.affected_rows_ = executed.affected_rows();
        result.insert_id_ = executed.last_insert_id();
    } catch (const ruvia::db_error& error) {
        record_failure(error, result);
    } catch (const std::invalid_argument&) {
        result.refused_ = true;
    }
}

inline ruvia::task<void> begin_and_commit(ruvia::db_session handle, outcome& result) {
    try {
        auto transaction = co_await handle.begin_transaction();
        co_await transaction.commit();
        result.value_ = "committed";
    } catch (const ruvia::db_error& error) {
        record_failure(error, result);
    }
}

}  // namespace pipeline_test
