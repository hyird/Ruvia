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

#include "app.h"
#include "context.h"
#include "http_server.h"
#include "middleware.h"
#include "worker_task_fixture.h"

#if defined(_WIN32)
#include <windows.h>

#include <cwchar>
#include <memory>
#endif
#include <optional>

#include "http2_connection.h"

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
    explicit tcp_port_reservation(asio::io_context& io) :
        socket_(io) {
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
    explicit udp_port_reservation(asio::io_context& io) :
        socket_(io) {
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

inline int sign_tls_certificate(X509* certificate, EVP_PKEY* key) {
    const auto context_value = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context_value || EVP_DigestSignInit_ex(context_value.get(), nullptr, "SHA256", nullptr, nullptr, key, nullptr) != 1) {
        return 0;
    }
    return X509_sign_ctx(certificate, context_value.get());
}

inline int write_tls_private_key(BIO* output, const EVP_PKEY* key, std::string_view password = {}, bool encrypted = false) {
    if (password.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return 0;
    }
    const auto cipher = std::unique_ptr<EVP_CIPHER, decltype(&EVP_CIPHER_free)>(encrypted ? EVP_CIPHER_fetch(nullptr, "AES-256-CBC", nullptr) : nullptr, EVP_CIPHER_free);
    if (encrypted && !cipher) {
        return 0;
    }
    const auto* bytes_value = reinterpret_cast<const unsigned char*>(password.empty() ? "" : password.data());
    return PEM_write_bio_PrivateKey_ex(output, key, cipher.get(), encrypted ? bytes_value : nullptr, static_cast<int>(password.size()), nullptr, nullptr, nullptr, nullptr);
}

class tls_identity final {
    scoped_temp_directory directory_{"ruvia-client-tls"};

public:
    // A self-signed identity whose subject CN and only DNS subjectAltName are `name`.
    explicit tls_identity(const char* name) :
        tls_identity(name, std::string("DNS:") + name) {}

    // The subject CN is `common_name`; `subject_alt_name` uses OpenSSL's
    // configuration syntax (`DNS:w*.example.com`), and empty omits the extension.
    tls_identity(const char* common_name, std::string_view subject_alt_name, bool elliptic_curve = false) :
        context_(asio::ssl::context::tls_server) {
        std::filesystem::permissions(directory_.path(), std::filesystem::perms::owner_all);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(elliptic_curve ? EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1") : EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", std::size_t{2048}), EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new_ex(nullptr, nullptr), X509_free);
        if (!key || !certificate || X509_set_version(certificate.get(), 2) != 1 || ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 || !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) || !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 86400) || X509_set_pubkey(certificate.get(), key.get()) != 1) {
            throw std::runtime_error("cannot generate TLS test identity");
        }
        const auto subject = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
        if (!subject || X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(common_name), -1, -1, 0) != 1 || X509_set_subject_name(certificate.get(), subject.get()) != 1 || X509_set_issuer_name(certificate.get(), subject.get()) != 1) {
            throw std::runtime_error("cannot name TLS test identity");
        }
        if (!subject_alt_name.empty()) {
            X509V3_CTX extension_context{};
            X509V3_set_ctx(&extension_context, certificate.get(), certificate.get(), nullptr, nullptr, 0);
            std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(X509V3_EXT_nconf_nid(nullptr, &extension_context, NID_subject_alt_name, std::string(subject_alt_name).c_str()), X509_EXTENSION_free);
            if (!extension || X509_add_ext(certificate.get(), extension.get(), -1) != 1) {
                throw std::runtime_error("cannot name TLS test identity");
            }
        }
        if (ruvia::test::sign_tls_certificate(certificate.get(), key.get()) <= 0 || SSL_CTX_use_certificate(context_.native_handle(), certificate.get()) != 1 || SSL_CTX_use_PrivateKey(context_.native_handle(), key.get()) != 1) {
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

// Startup hooks remain caller-owned: application retains them across runs.
class app_run final {
public:
    explicit app_run(application& app) :
        app_(app),
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

[[nodiscard]] inline std::string exchange(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view request) {
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    asio::write(socket, asio::buffer(request));
    return read_to_end(socket);
}

template <typename predicate_type, typename representation_type, typename period_type>
inline void wait_until(predicate_type predicate, std::chrono::duration<representation_type, period_type> timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("test observation timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

class http_server_fixture final {
public:
    // The application is shared by every case in the executable, so the first
    // fixture constructed fixes its server_config.
    explicit http_server_fixture(asio::io_context& io, server_config config = {.worker_count_ = 1, .connection_scan_interval_ = std::chrono::milliseconds(5)}) :
        runtime_(&shared_runtime(io, config)) {}

    [[nodiscard]] const asio::ip::tcp::endpoint& endpoint() const noexcept {
        return runtime_->endpoint();
    }

    // Call after closing or shutting down every client belonging to this case.
    // The application stays running for subsequent cases in the same executable.
    void finish() {
        runtime_->finish_case();
    }

private:
    class server_runtime final {
    public:
        server_runtime(asio::io_context& io, const server_config& config) :
            reservation_(io),
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
                    return ready.wait_for(std::chrono::seconds(0)) == std::future_status::ready || runner_->failure() != nullptr;
                }, std::chrono::seconds(5));
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
            }, std::chrono::seconds(5));
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

    static server_runtime& shared_runtime(asio::io_context& io, const server_config& config) {
        static server_runtime runtime(io, config);
        return runtime;
    }

    server_runtime* runtime_;
};

template <typename callback_type>
class context_callback final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    explicit context_callback(callback_type* callback) noexcept :
        callback_(callback) {}

    ruvia::task<void> invoke(ruvia::context& context_value, ruvia::next&) {
        co_await (*callback_)(context_value);
    }

private:
    callback_type* callback_;
};

template <typename callback_type>
[[nodiscard]] inline ruvia::dispatch_response with_context(const ruvia::test_request& request, callback_type&& callback) {
    ruvia::test_app app;
    app.use<context_callback<std::remove_reference_t<callback_type>>>(&callback);
    return app.request(request);
}

// Stateful, isolated RESP store. All map access and connection teardown run on
// its own event loop. Expiry is not simulated: these cases do not test time.
class redis_session_store_peer final {
public:
    explicit redis_session_store_peer(std::map<std::string, std::string> store = std::map<std::string, std::string>()) :
        acceptor_(io_, {asio::ip::address_v4::loopback(), 0}),
        work_(asio::make_work_guard(io_)),
        store_(std::move(store)) {
        asio::co_spawn(io_, accept(), [this](std::exception_ptr failure) {
            fail(failure);
        });
        thread_ = std::thread([this] {
            io_.run();
        });
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
        return error == asio::error::eof || error == asio::error::connection_reset || error == asio::error::connection_aborted || (closing_ && (error == asio::error::operation_aborted || error == asio::error::bad_descriptor));
    }

    asio::awaitable<void> accept() {
        try {
            for (;;) {
                auto socket = std::make_shared<socket_type>(io_);
                co_await acceptor_.async_accept(*socket, asio::use_awaitable);
                sockets_.push_back(socket);
                asio::co_spawn(io_, serve(socket), [this](std::exception_ptr failure) {
                    fail(failure);
                });
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
                co_await asio::async_read(socket, asio::dynamic_buffer(buffer), asio::transfer_exactly(size + 2 - buffer.size()), asio::use_awaitable);
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

inline void append_frame(std::string& wire, http2_frame_type type, std::uint8_t flags, std::uint32_t stream_id, std::string_view payload) {
    std::array<char, http2_frame_header_bytes> header{};
    if (payload.size() > std::numeric_limits<std::uint32_t>::max() || !encode_http2_frame_header(header, static_cast<std::uint32_t>(payload.size()), type, flags, stream_id)) {
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
[[nodiscard]] inline received_frame read_frame(stream_type& stream, std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt) {
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

#if defined(RUVIA_TEST_CLIENT_OPERATIONS)
#include "controller.h"
#include "http_client.h"
#include "stop_token.h"
#include "streaming.h"
#include "task.h"
#include "task_scope.h"

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
[[nodiscard]] inline ruvia::task<void> check_upload_abort(ruvia::http_client_ref client, const ruvia::worker_ref& worker, bool cancel) {
    auto exchange = co_await client.post("/operation/held-upload").upload({.expectation_ = ruvia::http_client_request_expectation::continue_value, .continue_timeout_ = 10s});
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
#endif
