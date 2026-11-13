#include "client/client_tcp_transport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/error.hpp>
#include <asio/write.hpp>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_wait.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/http_client_types.h"

#include "http/operation_interrupt.h"
#include "server/connection_lifecycle.h"

namespace ruvia::detail {

bool is_tls_error(const std::error_code& error) noexcept {
    return error.category() == asio::error::get_ssl_category() || error == asio::ssl::error::stream_truncated;
}

client_tcp_transport::client_tcp_transport(asio::io_context& io, asio::ssl::context& tls,
    const worker_ref& worker_value, client_tcp_endpoint endpoint, client_wire_counters& counters)
    : worker_(worker_value),
      tls_(tls),
      endpoint_(endpoint),
      literal_endpoint_([&]() -> std::optional<asio::ip::tcp::endpoint> {
          if (const auto address = parse_ip_address(endpoint_.host_)) {
              return asio::ip::tcp::endpoint(*address, endpoint_.port_);
          }
          return std::nullopt;
      }()),
      counters_(counters),
      resolver_(worker_value),
      stream_(io.get_executor(), tls) {
    // Owners build their transports eagerly, before any member that must be
    // retired, so a host this worker cannot look up rejects the client itself.
    require_client_host_lookup(endpoint_.host_, worker_);
}

client_tcp_transport::~client_tcp_transport() = default;

void client_tcp_transport::abort(client_abort_reason reason) noexcept {
    // The latest owner decision wins: a pool shutdown reports closing even if
    // a request cancellation reached the transport first.
    if (reason != client_abort_reason::none) {
        abort_reason_ = reason;
    }
    if (writing_ || reason == client_abort_reason::timeout || reason == client_abort_reason::cancelled) {
        tls_close_ready_ = false;
    }
    resolver_.cancel();
    std::error_code ignored;
    if (connecting_ != nullptr) {
        connecting_->cancel();
    } else {
        (void)stream_.lowest_layer().cancel(ignored);
    }
}

void client_tcp_transport::close() noexcept {
    read_.timer_.cancel();
    read_.deadline_.reset();
    write_.timer_.cancel();
    write_.deadline_.reset();
    resolver_.cancel();
    if (connecting_ != nullptr) {
        connecting_->cancel();
    }
    send_close_notify();
    std::error_code ignored;
    (void)stream_.lowest_layer().cancel(ignored);
    (void)stream_.lowest_layer().close(ignored);
}

void client_tcp_transport::send_close_notify() noexcept {
    if (!tls_close_ready_ || writing_) {
        return;
    }
    tls_close_ready_ = false;
    auto& layer = stream_.next_layer();
    auto& socket = layer.socket();
    SSL* const native = stream_.native_handle();
    // One worker runs every engine call, so the engine orders the alert after
    // the records of the pending read and write. On the socket it must follow
    // them whole too: while a record write waits for the socket or ended
    // inside a record (from a write, or from a read answering a post-handshake
    // message) the alert would land inside that record, and after a failed or
    // cancelled write it would follow a cut or lost one, so the close goes
    // without the alert. A post-handshake message still being read keeps the
    // engine in init.
    if (!layer.records_whole() || !socket.is_open() || SSL_is_init_finished(native) != 1 ||
        (SSL_get_shutdown(native) & SSL_SENT_SHUTDOWN) != 0) {
        return;
    }
    // Send our alert without waiting for the peer's: recording its alert as
    // received makes SSL_shutdown complete after the one write on the
    // non-blocking socket, which first flushes engine output an operation
    // left unsent. The engine is never read again; a full socket buffer fails
    // the write and the close proceeds without the alert.
    SSL_set_shutdown(native, SSL_get_shutdown(native) | SSL_RECEIVED_SHUTDOWN);
    std::error_code error;
    (void)stream_.shutdown(error);
}

// An idle pooled connection is reusable only while the peer has sent nothing:
// unsolicited bytes are a stray response or the start of a close. The probe is
// a constant-time inspection that never runs the TLS engine: a decrypting read
// on the worker keeps processing records for as long as the peer keeps sending
// post-handshake control records (TLS 1.3 NewSessionTicket, KeyUpdate), and
// nonblocking mode does not bound that loop. Without decrypting, those records
// cannot be told from a close_notify or a stray response, so any record
// OpenSSL already buffers (decrypted or not) or any byte waiting on the socket
// retires the connection. Servers send their tickets right after the
// handshake, ahead of the first response, so the response read consumes them
// and an idle connection normally carries none; a later control record only
// costs one reconnect. Raw bytes Asio holds in its own input buffer stay
// invisible here, like a close racing the probe: the response read reports them.
client_idle_transport_status client_tcp_transport::idle_status() noexcept {
    auto& socket = stream_.next_layer().socket();
    if (!socket.is_open()) {
        return client_idle_transport_status::closed;
    }
    if (secure()) {
        SSL* const native = stream_.native_handle();
        if (SSL_has_pending(native) != 0 || BIO_ctrl_pending(SSL_get_rbio(native)) != 0) {
            return client_idle_transport_status::pending_data;
        }
    }
    std::error_code error;
    char byte{};
    const auto bytes = socket.receive(
        asio::buffer(&byte, 1), asio::socket_base::message_peek, error);
    if (error == asio::error::would_block) {
        return client_idle_transport_status::reusable;
    }
    return !error && bytes != 0 ? client_idle_transport_status::pending_data
                                : client_idle_transport_status::closed;
}

task<void> client_tcp_transport::wait_readable() {
    // A readiness wait consumes nothing, so a readiness that races the next
    // exchange leaves its bytes to that exchange's read.
    (void)co_await async_asio([this](auto handler) mutable {
        stream_.next_layer().socket().async_wait(asio::socket_base::wait_read, std::move(handler));
    });
}

void client_tcp_transport::cancel_wait() noexcept {
    std::error_code ignored;
    (void)stream_.next_layer().socket().cancel(ignored);
}

bool client_tcp_transport::arm(lane_deadline& lane, const operation_deadline& timeout) {
    lane.timer_.cancel();
    const auto remaining = timeout.remaining();
    if (!remaining) {
        lane.deadline_.reset();
        return true;
    }
    if (remaining->count() == 0) {
        lane.deadline_.reset();
        return false;
    }
    const auto deadline_value = *timeout.deadline();
    lane.deadline_.arm(deadline_value);
    (void)worker_.schedule_timer(lane.timer_, deadline_value, [this, &lane](worker_timer_outcome outcome) noexcept {
        if (outcome == worker_timer_outcome::expired && lane.deadline_.expire(std::chrono::steady_clock::now())) {
            abort(client_abort_reason::timeout);
        }
    });
    return true;
}

bool client_tcp_transport::clear(lane_deadline& lane) noexcept {
    lane.timer_.cancel();
    return lane.deadline_.clear();
}

client_transport_result client_tcp_transport::aborted() const noexcept {
    return {.status_ = client_transport_status::aborted,
        .abort_ = abort_reason_ == client_abort_reason::none ? client_abort_reason::timeout : abort_reason_};
}

client_transport_result client_tcp_transport::finish(lane_deadline& lane, const operation_deadline& timeout,
    const std::error_code& error, std::size_t bytes, client_transport_status failure) noexcept {
    if ((clear(lane) || timeout.expired()) && abort_reason_ == client_abort_reason::none) {
        abort_reason_ = client_abort_reason::timeout;
    }
    if (abort_reason_ != client_abort_reason::none) {
        tls_close_ready_ = false;
        return aborted();
    }
    if (error) {
        if (failure == client_transport_status::io_failed && is_stream_read_end(error)) {
            return {.status_ = client_transport_status::eof, .error_ = error};
        }
        const auto status = secure() && tls_started_ && is_tls_error(error) ? client_transport_status::tls_failed : failure;
        return {.status_ = status, .bytes_ = bytes, .error_ = error};
    }
    return {.bytes_ = bytes};
}

task<client_transport_result> client_tcp_transport::connect(const operation_deadline& timeout) {
    const auto connect_started = std::chrono::steady_clock::now();
    connect_duration_ = {};
    tls_duration_ = {};
    peer_endpoint_ = {};
    tls_close_ready_ = false;
    writing_ = false;
    if (tls_started_) {
        // Closing a socket leaves Asio's encrypted input cursor and BIO pair
        // alive. Swap through a fresh stream so the retired SSL engine is
        // destroyed without overwriting its native owner, and the socket
        // layer's record accounting starts over.
        asio::ssl::stream<client_tls_socket> fresh(stream_.get_executor(), tls_);
        std::swap(stream_, fresh);
        tls_started_ = false;
    } else {
        stream_.next_layer().reset();
    }
    if (!arm(read_, timeout)) {
        co_return aborted();
    }
    if (abort_reason_ != client_abort_reason::none || read_.deadline_.expired() || timeout.expired()) {
        co_return finish(read_, timeout, {}, 0, client_transport_status::resolve_failed);
    }
    client_resolve_result resolved;
    if (endpoint_.resolve_) {
        resolved = co_await endpoint_.resolve_(endpoint_.resolve_context_, resolver_, timeout, endpoint_.host_, endpoint_.port_);
    } else if (literal_endpoint_) {
        resolved.literal_ = literal_endpoint_;
    } else {
        auto pending = resolver_.resolve(endpoint_.host_, endpoint_.port_, timeout);
        if (auto* immediate = std::get_if<client_resolve_result>(&pending)) {
            resolved = std::move(*immediate);
        } else {
            resolved = co_await std::move(std::get<task<client_resolve_result>>(pending));
        }
    }
    if (abort_reason_ != client_abort_reason::none || read_.deadline_.expired() || timeout.expired()) {
        co_return finish(read_, timeout, {}, 0, client_transport_status::resolve_failed);
    }
    if (resolved.error_) {
        co_return finish(read_, timeout, resolved.error_, 0, client_transport_status::resolve_failed);
    }
    if (resolved.literal_) {
        // One numeric endpoint needs neither a resolver result allocation nor
        // the multi-address race. The same transport deadline and cancellation
        // own this socket as in the ordinary I/O paths.
        stream_.next_layer().replace_socket(asio::ip::tcp::socket(stream_.get_executor()));
        const auto connected = co_await async_asio([this, endpoint = *resolved.literal_](auto handler) mutable {
            stream_.next_layer().socket().async_connect(endpoint, std::move(handler));
        });
        if (connected.error_code() || abort_reason_ != client_abort_reason::none ||
            read_.deadline_.expired() || timeout.expired()) {
            co_return finish(read_, timeout, connected.error_code(), 0, client_transport_status::connect_failed);
        }
    } else {
        client_happy_eyeballs connector(worker_, stream_.get_executor(), resolved.addresses_);
        resolved = {};
        connecting_ = &connector;
        struct clear_connector final {
            client_happy_eyeballs*& value_;
            ~clear_connector() {
                value_ = nullptr;
            }
        } connector_guard{connecting_};
        auto connected = co_await connector.connect();
        connecting_ = nullptr;
        if (connected.error_ || abort_reason_ != client_abort_reason::none || read_.deadline_.expired() ||
            timeout.expired()) {
            co_return finish(read_, timeout, connected.error_, 0, client_transport_status::connect_failed);
        }
        if (!connected.socket_) {
            std::terminate();
        }
        stream_.next_layer().replace_socket(std::move(*connected.socket_));
    }
    connect_duration_ = std::chrono::steady_clock::now() - connect_started;
    std::error_code peer_error;
    peer_endpoint_ = stream_.next_layer().socket().remote_endpoint(peer_error);
    if (peer_error) {
        co_return finish(read_, timeout, peer_error, 0, client_transport_status::connect_failed);
    }
    auto mode_error = apply_tcp_socket_policies(stream_.next_layer().socket(), endpoint_.tcp_);
    // Every socket operation is asynchronous or a bounded probe, and the TLS
    // socket layer writes records directly.
    if (!mode_error) {
        stream_.next_layer().socket().non_blocking(true, mode_error);
    }
    if (mode_error || !secure()) {
        co_return finish(read_, timeout, mode_error, 0, client_transport_status::connect_failed);
    }
    tls_started_ = true;
    if (const auto prepared =
            prepare_client_tls_stream(*stream_.native_handle(), endpoint_.identity_host_.empty() ? endpoint_.tls_->tls_host(endpoint_.host_) : endpoint_.identity_host_, endpoint_.alpn_);
        prepared != client_tls_setup_error::none) {
        (void)clear(read_);
        co_return client_transport_result{.status_ = client_transport_status::tls_failed,
            .error_ = make_client_tls_setup_error_code(prepared)};
    }
    const auto tls_started = std::chrono::steady_clock::now();
    const auto handshake = co_await async_asio([this](auto handler) mutable {
        stream_.async_handshake(asio::ssl::stream_base::client, std::move(handler));
    });
    tls_duration_ = std::chrono::steady_clock::now() - tls_started;
    auto result_value = finish(read_, timeout, handshake.error_code(), 0, client_transport_status::tls_failed);
    // RFC 9113 section 9.2: even a noncompliant peer selecting h2 over a
    // legacy TLS connection must never cause HTTP/2 preface/application I/O.
    // Enforce this here as raw TCP clients can also offer h2.
    if (result_value.ok() && alpn() == "h2" && SSL_version(stream_.native_handle()) < TLS1_2_VERSION) {
        result_value.status_ = client_transport_status::tls_failed;
        result_value.error_ = make_client_tls_setup_error_code(client_tls_setup_error::protocol_failed);
    }
    tls_close_ready_ = result_value.ok();
    co_return result_value;
}

std::string_view client_tcp_transport::alpn() noexcept {
    return tls_started_ ? tls_selected_alpn(*stream_.native_handle()) : std::string_view{};
}

task<client_transport_result> client_tcp_transport::read_some(std::span<char> bytes_value, const operation_deadline& timeout) {
    if (abort_reason_ != client_abort_reason::none || !arm(read_, timeout)) {
        co_return aborted();
    }
    const auto buffer = asio::buffer(bytes_value);
    const auto completion = secure()
                                ? co_await async_asio<std::size_t>([this, buffer](auto handler) mutable {
                                      stream_.async_read_some(buffer, std::move(handler));
                                  })
                                : co_await async_asio<std::size_t>([this, buffer](auto handler) mutable {
                                      stream_.next_layer().socket().async_read_some(buffer, std::move(handler));
                                  });
    // After a TLS failure the engine owes the peer no further records.
    if (is_tls_error(completion.error_code())) {
        tls_close_ready_ = false;
    }
    const auto transferred = completion.result();
    counters_.received_ += transferred;
    auto result_value = finish(read_, timeout, completion.error_code(),
        completion.error_code() ? 0 : transferred, client_transport_status::io_failed);
    if (result_value.ok() && result_value.bytes_ == 0) {
        result_value.status_ = client_transport_status::eof;
    }
    co_return result_value;
}

template <typename buffers_type>
task<client_transport_result> client_tcp_transport::write_buffers(buffers_type buffers, const operation_deadline& timeout) {
    if (abort_reason_ != client_abort_reason::none) {
        co_return aborted();
    }
    if (asio::buffer_size(buffers) == 0) {
        co_return client_transport_result{};
    }
    if (!arm(write_, timeout)) {
        tls_close_ready_ = false;
        co_return aborted();
    }
    struct write_guard final {
        bool& writing_;
        explicit write_guard(bool& writing) noexcept
            : writing_(writing) {
            writing_ = true;
        }
        ~write_guard() noexcept {
            writing_ = false;
        }
    } guard(writing_);
    const auto completion = secure()
                                ? co_await async_asio<std::size_t>([this, buffers](auto handler) mutable {
                                      asio::async_write(stream_, buffers, std::move(handler));
                                  })
                                : co_await async_asio<std::size_t>([this, buffers](auto handler) mutable {
                                      asio::async_write(stream_.next_layer().socket(), buffers, std::move(handler));
                                  });
    if (completion.error_code()) {
        tls_close_ready_ = false;
    }
    const auto transferred = completion.result();
    counters_.sent_ += transferred;
    auto result_value = finish(write_, timeout, completion.error_code(),
        completion.error_code() ? 0 : transferred, client_transport_status::io_failed);
    if (result_value.status_ == client_transport_status::eof) {
        result_value.status_ = client_transport_status::io_failed;
    }
    co_return result_value;
}

task<client_transport_result> client_tcp_transport::write(std::string_view bytes_value, const operation_deadline& timeout) {
    return write_buffers(asio::buffer(bytes_value), timeout);
}

task<client_transport_result> client_tcp_transport::write(
    std::span<const asio::const_buffer> buffers, const operation_deadline& timeout) {
    return write_buffers(buffers, timeout);
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

struct client_tls_preparation final {
    explicit client_tls_preparation(client_tls_config_storage config)
        : config_(std::move(config)),
          password_() {}
    ~client_tls_preparation() {
        if (!password_.empty()) {
            OPENSSL_cleanse(password_.data(), password_.size());
        }
    }
    client_tls_config_storage config_;
    std::string password_;
};

task<client_tls_config_storage> prepare_client_tls_owned(worker_ref worker,
    std::unique_ptr<client_tls_preparation> input,
    stop_token stop) {
    auto result = co_await run_blocking(std::move(worker),
        [input = std::move(input), stop = std::move(stop)]() mutable {
            if (stop.stop_requested()) {
                throw std::runtime_error("client TLS preparation cancelled");
            }
            input->config_.parse_files(input->password_);
            return std::move(input->config_);
        });
    co_return std::move(result).take_value();
}

constexpr std::array<unsigned char, 9> http11_alpn = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
constexpr std::array<unsigned char, 3> http2_alpn = {2, 'h', '2'};
constexpr std::array<unsigned char, 12> negotiated_http_alpn = {
    2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

[[nodiscard]] bool configure_client_tls_peer_identity(SSL& connection, const char* host, bool ip_address) noexcept {
    X509_VERIFY_PARAM* const parameters = SSL_get0_param(&connection);
    if (ip_address) {
        return X509_VERIFY_PARAM_set1_host(parameters, nullptr, 0) == 1 &&
               X509_VERIFY_PARAM_set1_ip_asc(parameters, host) == 1;
    }
    // RFC 9110 §4.3.4 / RFC 9525 §6.3: DNS-ID only, never the subject Common
    // Name, and a wildcard only as the complete left-most label.
    X509_VERIFY_PARAM_set_hostflags(parameters, X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    return X509_VERIFY_PARAM_set1_ip(parameters, nullptr, 0) == 1 &&
           X509_VERIFY_PARAM_set1_host(parameters, host, 0) == 1;
}

// Prepares SNI and the expected peer identity; TLS always authenticates the peer.
[[nodiscard]] client_tls_setup_error prepare_client_tls_session(SSL& ssl, std::string_view host) noexcept {
    const bool is_ip_address = is_client_ip_address(host);
    if (!is_ip_address && host.ends_with('.')) {
        host.remove_suffix(1);
    }
    std::array<char, 256> tls_host_buffer;
    if (host.size() >= tls_host_buffer.size()) {
        return client_tls_setup_error::sni_failed;
    }
    std::ranges::copy(host, tls_host_buffer.begin());
    tls_host_buffer[host.size()] = '\0';
    // RFC 6066 HostName carries a DNS host_name, never an IPv4/IPv6 literal.
    // Host verification still receives IP literals so OpenSSL can validate IP
    // subjectAltName entries.
    if (!is_ip_address && SSL_set_tlsext_host_name(&ssl, tls_host_buffer.data()) != 1) {
        return client_tls_setup_error::sni_failed;
    }
    if (!configure_client_tls_peer_identity(ssl, tls_host_buffer.data(), is_ip_address)) {
        return client_tls_setup_error::peer_identity_failed;
    }
    return client_tls_setup_error::none;
}

[[nodiscard]] std::string_view client_tls_setup_error_message(client_tls_setup_error error) noexcept {
    switch (error) {
        case client_tls_setup_error::none:
            return {};
        case client_tls_setup_error::reset_failed:
            return "failed to reset TLS stream";
        case client_tls_setup_error::sni_failed:
            return "failed to set TLS SNI host";
        case client_tls_setup_error::alpn_failed:
            return "failed to configure TLS ALPN";
        case client_tls_setup_error::peer_identity_failed:
            return "failed to configure TLS peer identity";
        case client_tls_setup_error::protocol_failed:
            return "TLS protocol requirements were not met";
    }
    std::terminate();
}

class client_tls_setup_category_type final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override {
        return "client TLS setup";
    }
    [[nodiscard]] std::string message(int value) const override {
        return std::string(client_tls_setup_error_message(static_cast<client_tls_setup_error>(value)));
    }
};

}  // namespace
[[nodiscard]] std::span<const unsigned char> client_alpn_bytes(client_alpn mode) noexcept {
    switch (mode) {
        case client_alpn::none:
            return {};
        case client_alpn::http11:
            return http11_alpn;
        case client_alpn::http2:
            return http2_alpn;
        case client_alpn::negotiate:
            return negotiated_http_alpn;
    }
    std::terminate();
}

client_tls_config_storage::client_tls_config_storage(
    const client_tls_config& source_value, client_tls_files files)
    : mode_(source_value.mode_),
      minimum_version_(source_value.minimum_version_),
      max_outgoing_records_(source_value.max_outgoing_records_) {
    if (source_value.ca_file_.has_value()) {
        ca_file_.emplace(make_tls_file_name(*source_value.ca_file_));
    }
    if (source_value.identity_.has_value()) {
        identity_.emplace(make_tls_identity_files(*source_value.identity_));
    }
    if (source_value.server_name_.has_value()) {
        server_name_.emplace(*source_value.server_name_);
    }
    if (files != client_tls_files::parse || mode_ == client_tls_mode::disabled) {
        return;
    }
    parse_files(source_value.identity_ ? std::string_view(source_value.identity_->private_key_password_) : std::string_view{});
}

void client_tls_config_storage::parse_files(std::string_view private_key_password) {
    // An owner may supply its shared system trust store; it is never re-read here.
    if (!trust_store_.has_value()) {
        trust_store_ = ca_file_.has_value() ? load_tls_trust_store(*ca_file_) : tls_trust_store::load_default();
    }
    if (identity_.has_value()) {
        certificate_ = load_tls_certificate(*identity_, private_key_password);
    }
}

task<client_tls_config_storage> prepare_client_tls_config(
    worker_ref worker, client_tls_config_storage config,
    std::string_view private_key_password, stop_token stop) {
    auto input = std::make_unique<client_tls_preparation>(std::move(config));
    // Keep the secret at one stable address: moving an SSO string can leave
    // uncleansed bytes behind in an abandoned coroutine or callable frame.
    input->password_.assign(private_key_password);
    return prepare_client_tls_owned(std::move(worker), std::move(input), std::move(stop));
}

client_tls_config_storage::client_tls_config_storage(
    const client_tls_config_storage& source_value)
    : mode_(source_value.mode_),
      minimum_version_(source_value.minimum_version_),
      max_outgoing_records_(source_value.max_outgoing_records_),
      trust_store_(source_value.trust_store_),
      certificate_(source_value.certificate_) {
    if (source_value.ca_file_.has_value()) {
        ca_file_.emplace(*source_value.ca_file_);
    }
    if (source_value.identity_.has_value()) {
        identity_.emplace(copy_tls_identity_files(*source_value.identity_));
    }
    if (source_value.server_name_.has_value()) {
        server_name_.emplace(*source_value.server_name_);
    }
}

void configure_client_tls_context(SSL_CTX& context_value, const client_tls_config_storage& config) {
    if (config.mode_ != client_tls_mode::verify_identity) {
        throw std::invalid_argument("client TLS context requires TLS to be enabled");
    }
    apply_tls_protocol_policy(context_value, config.max_outgoing_records_);
    if (SSL_CTX_set_min_proto_version(&context_value, static_cast<int>(config.minimum_version_)) != 1) {
        throw std::runtime_error("failed to configure client TLS minimum version");
    }
    SSL_CTX_set_verify(&context_value, SSL_VERIFY_PEER, nullptr);
    if (!config.trust_store_.has_value()) {
        throw std::logic_error("client TLS trust store was not parsed");
    }
    config.trust_store_->install(context_value);
    if (!config.identity_.has_value()) {
        return;
    }
    if (!config.certificate_.has_value()) {
        throw std::logic_error("client TLS certificate was not parsed");
    }
    config.certificate_->install(context_value);
}

client_tls_setup_error prepare_client_tls_stream(SSL& ssl, std::string_view host, std::span<const unsigned char> protocols) {
    if (SSL_clear(&ssl) != 1) {
        return client_tls_setup_error::reset_failed;
    }
    // Keep a stricter context minimum (in particular QUIC's TLS 1.3).
    // Mixed ALPN offers may negotiate HTTP/1.1 over TLS 1.1; the shared TCP
    // transport checks the selected protocol before exposing any byte I/O.
    if (std::ranges::equal(protocols, http2_alpn) &&
        SSL_get_min_proto_version(&ssl) < TLS1_2_VERSION &&
        SSL_set_min_proto_version(&ssl, TLS1_2_VERSION) != 1) {
        return client_tls_setup_error::protocol_failed;
    }
    if (const auto prepared = prepare_client_tls_session(ssl, host); prepared != client_tls_setup_error::none) {
        return prepared;
    }
    if (SSL_set_alpn_protos(&ssl, protocols.data(), static_cast<unsigned int>(protocols.size())) != 0) {
        return client_tls_setup_error::alpn_failed;
    }
    return client_tls_setup_error::none;
}

std::error_code make_client_tls_setup_error_code(client_tls_setup_error error) noexcept {
    static const client_tls_setup_category_type category;
    return {static_cast<int>(error), category};
}

}  // namespace ruvia::detail

namespace ruvia::detail {

client_happy_eyeballs::client_happy_eyeballs(const worker_ref& worker,
    const asio::ip::tcp::socket::executor_type& executor,
    const asio::ip::tcp::resolver::results_type& addresses)
    : worker_(worker),
      changed_(worker),
      attempts_() {
    if (addresses.empty()) {
        return;
    }
    attempts_.reserve(addresses.size());
    client_address_sequence sequence(addresses);
    while (auto endpoint = sequence.next()) {
        attempts_.emplace_back(executor, *endpoint);
    }
}

client_happy_eyeballs::~client_happy_eyeballs() {
    delay_.cancel();
    if (active_ != 0) {
        std::terminate();
    }
}

void client_happy_eyeballs::close_socket(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
    (void)socket.cancel(ignored);
    (void)socket.close(ignored);
}

void client_happy_eyeballs::close_active() noexcept {
    for (auto& current : attempts_) {
        if (current.active_) {
            close_socket(current.socket_);
        }
    }
}

void client_happy_eyeballs::cancel() noexcept {
    if (cancelled_) {
        return;
    }
    cancelled_ = true;
    if (winner_) {
        close_socket(attempts_[*winner_].socket_);
    }
    winner_.reset();
    delay_.cancel();
    close_active();
    changed_.notify();
}

void client_happy_eyeballs::schedule_next() noexcept {
    if (next_ == attempts_.size() || cancelled_ || winner_) {
        return;
    }
    try {
        const auto status = worker_.schedule_timer(delay_, deadline_after(client_connection_attempt_delay),
            [this](worker_timer_outcome outcome) noexcept { on_delay(outcome); });
        if (status != worker_timer_schedule_status::worker_stopping) {
            return;
        }
    } catch (...) {
        failure_ = std::current_exception();
    }
    cancel();
}

void client_happy_eyeballs::start_next() noexcept {
    if (next_ == attempts_.size() || cancelled_ || winner_) {
        return;
    }
    const auto index = next_++;
    auto& current = attempts_[index];
    current.active_ = true;
    ++active_;
    try {
        current.socket_.async_connect(current.endpoint_,
            [this, index](const std::error_code& error) noexcept { on_connect(index, error); });
    } catch (...) {
        current.active_ = false;
        --active_;
        close_socket(current.socket_);
        failure_ = std::current_exception();
        cancel();
        return;
    }
    schedule_next();
}

void client_happy_eyeballs::on_delay(worker_timer_outcome outcome) noexcept {
    if (outcome == worker_timer_outcome::cancelled) {
        cancel();
        return;
    }
    launch_ready_ = true;
    changed_.notify();
}

void client_happy_eyeballs::on_connect(std::size_t index, const std::error_code& error) noexcept {
    auto& current = attempts_[index];
    if (!current.active_ || active_ == 0) {
        std::terminate();
    }
    current.active_ = false;
    --active_;
    if (!error && !cancelled_ && !winner_) {
        winner_ = index;
        delay_.cancel();
        close_active();
    } else {
        if (error && error != asio::error::operation_aborted) {
            last_error_ = error;
            if (!cancelled_ && !winner_) {
                // RFC 8305 section 5: a fast failure must not leave the next
                // address waiting for the remainder of the attempt delay.
                delay_.cancel();
                launch_ready_ = true;
            }
        }
        close_socket(current.socket_);
    }
    changed_.notify();
}

task<client_happy_eyeballs_result> client_happy_eyeballs::connect() & {
    if (cancelled_) {
        co_return client_happy_eyeballs_result{{}, asio::error::operation_aborted};
    }
    if (attempts_.empty()) {
        co_return client_happy_eyeballs_result{{}, asio::error::host_not_found};
    }
    start_next();
    for (;;) {
        if (launch_ready_) {
            launch_ready_ = false;
            start_next();
        }
        if (cancelled_) {
            close_active();
            if (active_ == 0) {
                if (failure_) {
                    std::rethrow_exception(failure_);
                }
                co_return client_happy_eyeballs_result{{}, asio::error::operation_aborted};
            }
        } else if (winner_) {
            if (active_ == 0) {
                auto socket = std::move(attempts_[*winner_].socket_);
                co_return client_happy_eyeballs_result{std::move(socket), {}};
            }
        } else if (active_ == 0 && next_ == attempts_.size()) {
            co_return client_happy_eyeballs_result{{},
                last_error_ ? last_error_ : std::error_code(asio::error::host_unreachable)};
        }
        co_await changed_.wait();
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {
struct client_host_resolver::query final {
    std::string host_;
    std::uint16_t port_;

    [[nodiscard]] client_resolve_result operator()() const {
        asio::io_context io;
        asio::ip::tcp::resolver resolver(io);
        std::array<char, http_max_unsigned_decimal_size> digits;
        const auto service = http_format_unsigned(digits, port_);
        client_resolve_result result;
        result.addresses_ = resolver.resolve(host_, service, result.error_);
        return result;
    }
};

bool client_host_needs_lookup(std::string_view host) {
    return !parse_ip_address(host).has_value();
}

void require_client_host_lookup(std::string_view host, const worker_ref& worker) {
    if (worker.blocking_pool() == nullptr && client_host_needs_lookup(host)) {
        throw std::invalid_argument("a DNS host requires an event loop with a blocking pool");
    }
}

client_host_resolver::client_host_resolver(client_host_resolver&& other) noexcept
    : worker_(other.worker_) {
    if (other.stop_) {
        std::terminate();
    }
}

client_host_resolver& client_host_resolver::operator=(client_host_resolver&& other) noexcept {
    if (stop_ || other.stop_) {
        std::terminate();
    }
    worker_ = other.worker_;
    return *this;
}

client_resolve_operation client_host_resolver::resolve(std::string_view host, std::uint16_t port,
    const operation_deadline& timeout, stop_token stop) {
    if (stop.stop_requested()) {
        return client_resolve_result{.error_ = asio::error::operation_aborted};
    }
    if (const auto address = parse_ip_address(host)) {
        return client_resolve_result{.literal_ = asio::ip::tcp::endpoint(*address, port)};
    }
    return resolve_query(query{std::string(host), port}, timeout, std::move(stop));
}

task<client_resolve_result> client_host_resolver::resolve_query(query input, operation_deadline timeout, stop_token stop) {
    if (stop_) {
        throw std::logic_error("host resolver already has a pending query");
    }
    stop_.emplace();
    struct reset_stop final {
        std::optional<stop_source>& stop_;
        ~reset_stop() {
            stop_.reset();
        }
    } guard{stop_};
    stop_group cancellation(stop_->token(), stop);
    auto result = co_await run_blocking(
        *worker_, {.timeout_ = timeout.timeout(), .stop_token_ = cancellation.token()}, std::move(input));
    switch (result.status()) {
        case blocking_status::completed:
            co_return std::move(result).take_value();
        case blocking_status::timed_out:
            co_return client_resolve_result{.error_ = asio::error::timed_out};
        case blocking_status::queue_full:
            co_return client_resolve_result{.error_ = asio::error::no_buffer_space};
        default:
            co_return client_resolve_result{.error_ = asio::error::operation_aborted};
    }
}

void client_host_resolver::cancel() noexcept {
    if (stop_) {
        stop_->request_stop();
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

[[nodiscard]] std::string owned_host(const tcp_client_config& config) {
    validate_client_origin_host(config.host_, "TCP host must not be empty", "TCP host is invalid");
    if (config.port_.value() == 0) {
        throw std::invalid_argument("TCP port must not be zero");
    }
    validate_client_tls_config(config.tls_);
    validate_client_tcp_config(config.tcp_);
    for (const auto& timeout : {config.connect_timeout_, config.read_timeout_, config.write_timeout_}) {
        if (timeout.has_value()) {
            // Each configured bound becomes an operation_deadline, which rejects non-positive values.
            static_cast<void>(operation_timeout(*timeout));
        }
    }
    return std::string(config.host_);
}

[[nodiscard]] std::vector<unsigned char> owned_alpn(const tcp_client_config& config) {
    if (!config.alpn_.empty() && config.tls_.mode_ == client_tls_mode::disabled) {
        throw std::invalid_argument("TCP ALPN requires TLS");
    }
    std::size_t size = 0;
    for (const auto& protocol : config.alpn_) {
        if (protocol.empty() || protocol.size() > 255) {
            throw std::invalid_argument("TLS ALPN names must contain 1..255 bytes");
        }
        if (65535 - size < protocol.size() + 1) {
            throw std::invalid_argument("TLS ALPN offer exceeds 65535 bytes");
        }
        size += protocol.size() + 1;
    }
    std::vector<unsigned char> wire;
    wire.reserve(size);
    for (const auto& protocol : config.alpn_) {
        wire.push_back(static_cast<unsigned char>(protocol.size()));
        const auto* bytes = reinterpret_cast<const unsigned char*>(protocol.data());
        wire.insert(wire.end(), bytes, bytes + protocol.size());
    }
    return wire;
}

[[nodiscard]] asio::ssl::context make_tls_context(const client_tls_config_storage& config) {
    asio::ssl::context context(asio::ssl::context::tls_client);
    if (config.mode_ != client_tls_mode::disabled) {
        configure_client_tls_context(*context.native_handle(), config);
    }
    return context;
}

[[noreturn]] void throw_transport_error(const client_transport_result& result) {
    std::error_code error = result.error_;
    if (result.status_ == client_transport_status::aborted) {
        error = std::make_error_code(result.abort_ == client_abort_reason::timeout ? std::errc::timed_out
                                                                                   : std::errc::operation_canceled);
    }
    if (!error) {
        error = std::make_error_code(std::errc::io_error);
    }
    throw std::system_error(error, "TCP client operation failed");
}
}  // namespace

class tcp_client_backend final {
public:
    tcp_client_backend(event_loop& loop, const worker_ref& worker, const tcp_client_config& config)
        : host_(owned_host(config)),
          alpn_(owned_alpn(config)),
          tls_config_(config.tls_, client_tls_files::parse),
          tls_context_(make_tls_context(tls_config_)),
          transport_(loop.io_context(), tls_context_, worker,
              {host_, config.port_.value(), tls_config_.mode_ == client_tls_mode::disabled ? nullptr : &tls_config_, config.tcp_, alpn_}, counters_),
          cancellation_(make_worker_cancellation_target(*this, worker)),
          connect_timeout_(config.connect_timeout_),
          read_timeout_(config.read_timeout_),
          write_timeout_(config.write_timeout_) {}

    ~tcp_client_backend() {
        cancellation_->detach(*this);
    }

    [[nodiscard]] task<void> connect() {
        // The lifecycle admits one startup; snapshot options before suspending.
        return connect_transport(std::move(connect_options_));
    }

    [[nodiscard]] std::string_view alpn() noexcept {
        return transport_.alpn();
    }

    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options options) {
        auto lease = claim_operation_lane(read_active_, "TCP client read already in progress");
        operation_deadline timeout(override_operation_options({.timeout_ = read_timeout_}, options).timeout_);
        worker_cancellation_registration cancellation(cancellation_, read_id_);
        cancellation.arm(options.stop_token_);
        if (transport_.abort_reason() != client_abort_reason::none) {
            throw_transport_error({.status_ = client_transport_status::aborted, .abort_ = transport_.abort_reason()});
        }
        if (eof_) {
            co_return std::nullopt;
        }
        const auto result = co_await transport_.read_some(buffer_, timeout);
        if (result.status_ == client_transport_status::eof) {
            eof_ = true;
            co_return std::nullopt;
        }
        if (!result.ok()) {
            throw_transport_error(result);
        }
        co_return as_bytes(std::string_view(buffer_.data(), result.bytes_));
    }

    [[nodiscard]] task<void> write(std::string bytes, operation_options options) {
        auto lease = claim_operation_lane(write_active_, "TCP client write already in progress");
        operation_deadline timeout(override_operation_options({.timeout_ = write_timeout_}, options).timeout_);
        worker_cancellation_registration cancellation(cancellation_, write_id_);
        cancellation.arm(options.stop_token_);
        const auto result = co_await transport_.write(bytes, timeout);
        if (!result.ok()) {
            throw_transport_error(result);
        }
    }

    void close_now() noexcept {
        transport_.abort(client_abort_reason::closing);
        transport_.close();
    }

    void cancel_operation_by_id(std::uint64_t id) noexcept {
        if (id != 0 && (id == connect_id_ || id == read_id_ || id == write_id_)) {
            transport_.abort(client_abort_reason::cancelled);
            // A composed plaintext write may have a successful completion
            // queued but no socket wait for cancel() to interrupt. Cancellation
            // retires this connection, so close it before that completion can
            // initiate another write, and cancel both lane deadlines as well.
            transport_.close();
        }
    }

    operation_options connect_options_;

private:
    [[nodiscard]] task<void> connect_transport(operation_options options) {
        operation_deadline timeout(override_operation_options({.timeout_ = connect_timeout_}, options).timeout_);
        worker_cancellation_registration cancellation(cancellation_, connect_id_);
        cancellation.arm(options.stop_token_);
        const auto result = co_await transport_.connect(timeout);
        if (!result.ok()) {
            throw_transport_error(result);
        }
    }

    std::string host_;
    std::vector<unsigned char> alpn_;
    client_tls_config_storage tls_config_;
    asio::ssl::context tls_context_;
    client_wire_counters counters_;
    client_tcp_transport transport_;
    std::shared_ptr<worker_cancellation_target<tcp_client_backend>> cancellation_;
    std::optional<timeout_duration> connect_timeout_;
    std::optional<timeout_duration> read_timeout_;
    std::optional<timeout_duration> write_timeout_;
    std::array<char, 4096> buffer_{};
    std::uint64_t connect_id_{};
    std::uint64_t read_id_{};
    std::uint64_t write_id_{};
    bool read_active_{};
    bool write_active_{};
    bool eof_{};
};

class tcp_client_state final : public std::enable_shared_from_this<tcp_client_state> {
public:
    tcp_client_state(event_loop loop, const tcp_client_config& config)
        : loop_(client_lifecycle<tcp_client_state>::require_loop(std::move(loop))),
          worker_(loop_.worker()),
          backend_(loop_, worker_, config),
          lifecycle_(*this, loop_, worker_) {}

    void bind_stop() {
        lifecycle_.bind_stop();
    }
    [[nodiscard]] task<void> connect(operation_options options) {
        return connect_owned(shared_from_this(), std::move(options));
    }
    [[nodiscard]] std::string_view alpn() {
        lifecycle_.require_ready();
        return backend_.alpn();
    }
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options options) {
        lifecycle_.require_ready();
        return make_scoped_operation(lifecycle_.operation_scope(), read_owned(shared_from_this(), std::move(options)));
    }
    [[nodiscard]] task<void> write(std::string bytes, operation_options options) {
        lifecycle_.require_ready();
        return make_scoped_operation(lifecycle_.operation_scope(), write_owned(shared_from_this(), std::move(bytes), std::move(options)));
    }
    void request_close() noexcept {
        lifecycle_.request_close();
    }
    [[nodiscard]] task<void> shutdown() {
        return lifecycle_.shutdown();
    }
    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

private:
    friend class client_lifecycle<tcp_client_state>;
    static constexpr std::string_view client_name{"TCP"};
    [[noreturn]] static void throw_not_ready() {
        throw std::system_error(std::make_error_code(std::errc::not_connected), "TCP client is not connected");
    }
    [[nodiscard]] tcp_client_backend& backend() noexcept {
        return backend_;
    }
    [[nodiscard]] static task<void> connect_owned(std::shared_ptr<tcp_client_state> state, operation_options options) {
        if (!state->worker_.is_current()) {
            throw std::logic_error("TCP client must connect on its bound event loop");
        }
        auto lease = claim_operation_lane(state->connecting_, "TCP client connect already in progress");
        state->backend_.connect_options_ = std::move(options);
        co_await state->lifecycle_.connect();
    }
    [[nodiscard]] static task<std::optional<std::span<const std::byte>>> read_owned(
        std::shared_ptr<tcp_client_state> state, operation_options options) {
        state->lifecycle_.require_ready();
        // Lane admission errors are caller errors, not transport failures.
        try {
            co_return co_await state->backend_.read(std::move(options));
        } catch (const std::system_error&) {
            state->request_close();
            throw;
        }
    }
    [[nodiscard]] static task<void> write_owned(std::shared_ptr<tcp_client_state> state,
        std::string bytes, operation_options options) {
        state->lifecycle_.require_ready();
        try {
            co_await state->backend_.write(std::move(bytes), std::move(options));
        } catch (const std::system_error&) {
            state->request_close();
            throw;
        }
    }

    event_loop loop_;
    worker_ref worker_;
    tcp_client_backend backend_;
    bool connecting_{};
    client_lifecycle<tcp_client_state> lifecycle_;
};

}  // namespace ruvia::detail

namespace ruvia {

tcp_client::tcp_client(event_loop loop, const tcp_client_config& config)
    : state_(std::make_shared<detail::tcp_client_state>(std::move(loop), config)) {
    state_->bind_stop();
}

tcp_client::~tcp_client() {
    state_->request_close();
}

task<void> tcp_client::connect(operation_options options) & {
    return state_->connect(std::move(options));
}

std::string_view tcp_client::alpn() const& {
    return state_->alpn();
}
task<std::optional<std::span<const std::byte>>> tcp_client::read(operation_options options) & {
    return state_->read(std::move(options));
}

task<void> tcp_client::write(std::span<const std::byte> bytes, operation_options options) & {
    return write_owned(std::string(as_chars(bytes)), std::move(options));
}

task<void> tcp_client::write(std::string_view bytes, operation_options options) & {
    return write_owned(std::string(bytes), std::move(options));
}

task<void> tcp_client::write_owned(std::string bytes, operation_options options) {
    return state_->write(std::move(bytes), std::move(options));
}

void tcp_client::close() noexcept {
    state_->request_close();
}

task<void> tcp_client::shutdown() & {
    return state_->shutdown();
}

const worker_ref& tcp_client::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
