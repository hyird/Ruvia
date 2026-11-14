#include "server_listener.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>

#include "app.h"
#include "app_state.h"
#include "crypto.h"
#include "http_server.h"
#include "server_options.h"
#include "startup_config.h"
#include "tls.h"
#include "tls_config.h"
#include "tls_server.h"

namespace ruvia::detail {

namespace {

// The identity set is immutable and shared, not copied.
http_server_listener_definition::tls clone_tls(const http_server_listener_definition::tls& source_value) {
    http_server_listener_definition::tls result_value(source_value.identities_);
    if (const auto& policy = source_value.client_certificates_; policy.has_value()) {
        result_value.client_certificates_.emplace(http_server_listener_definition::tls_client_certificate_policy{
            .requirement_ = policy->requirement_,
            .client_ca_ = policy->client_ca_,
            .ca_file_ = tls_file_name(policy->ca_file_),
        });
    }
    result_value.max_outgoing_records_ = source_value.max_outgoing_records_;
    result_value.session_ticket_keys_ = source_value.session_ticket_keys_;
    result_value.tcp_mode_ = source_value.tcp_mode_;
    result_value.plaintext_redirect_ = source_value.plaintext_redirect_;
    return result_value;
}

http_server_listener_definition::transport_type clone_transport(const http_server_listener_definition::transport_type& source_value) {
    return std::visit([]<typename transport_type>(const transport_type& transport) -> http_server_listener_definition::transport_type {
        if constexpr (std::is_same_v<transport_type, http_server_listener_definition::tls>) {
            return clone_tls(transport);
        } else {
            return transport;
        }
    }, source_value);
}

std::optional<http_server_listener_definition::connection_dispatch> clone_connection_dispatch(const std::optional<http_server_listener_definition::connection_dispatch>& source_value) {
    std::optional<http_server_listener_definition::connection_dispatch> result_value;
    if (source_value.has_value()) {
        result_value.emplace(source_value->alpn_);
    }
    return result_value;
}

}  // namespace

http_server_listener_definition http_server_listener_definition::clone() const {
    http_server_listener_definition result_value(endpoint_, clone_transport(transport_), http3_);
    result_value.connection_ = clone_connection_dispatch(connection_);
    result_value.inherited_sockets_ = inherited_sockets_;
    result_value.ipv6_only_ = ipv6_only_;
    result_value.proxy_protocol_ = proxy_protocol_;
    result_value.request_stream_ = request_stream_;
    return result_value;
}

http_server_session_config::http_server_session_config(const http_server_listener_definition& definition) :
    transport_(clone_transport(definition.transport_)),
    ipv6_only_(definition.ipv6_only_),
    proxy_protocol_(definition.proxy_protocol_),
    http3_(definition.http3_),
    connection_(clone_connection_dispatch(definition.connection_)),
    request_stream_(definition.request_stream_) {
    if (http3_.has_value()) {
        // RFC 7838 §3: alt-authority with an empty host names this origin's
        // host on the UDP port; "ma" bounds how long clients cache it.
        alternative_service_.append("h3=\":");
        alternative_service_.append(std::to_string(http3_->port_));
        alternative_service_.append("\"; ma=");
        alternative_service_.append(std::to_string(http3_->alt_svc_max_age_.count()));
    }
}

void http_server_session_config::adopt_tls_identities(std::shared_ptr<const http_server_tls_identities> identities) noexcept {
    if (auto* tls = std::get_if<http_server_listener_definition::tls>(&transport_)) {
        tls->identities_.swap(identities);
    }
}

namespace {

#if defined(_WIN32)
using socket_length = int;

[[nodiscard]] SOCKET os_socket(native_socket socket) noexcept {
    return static_cast<SOCKET>(socket);
}
#else
using socket_length = socklen_t;

[[nodiscard]] int os_socket(native_socket socket) noexcept {
    return socket;
}
#endif

// The integer SOL_SOCKET option `name`; nullopt when `socket` is no socket.
[[nodiscard]] std::optional<int> socket_option(native_socket socket, int name) noexcept {
    int value = 0;
    auto length = static_cast<socket_length>(sizeof(value));
    if (::getsockopt(os_socket(socket), SOL_SOCKET, name, reinterpret_cast<char*>(&value), &length) != 0) {
        return std::nullopt;
    }
    return value;
}

// Whether `socket` is bound to exactly `endpoint`, in its address family.
template <typename endpoint_type>
[[nodiscard]] bool bound_to(native_socket socket, const endpoint_type& endpoint) {
    endpoint_type local;
    auto length = static_cast<socket_length>(local.capacity());
    if (::getsockname(os_socket(socket), local.data(), &length) != 0 || length <= 0 || static_cast<std::size_t>(length) > local.capacity() || local.data()->sa_family != endpoint.data()->sa_family) {
        return false;
    }
    local.resize(static_cast<std::size_t>(length));
    return local == endpoint;
}

// Inherited handles leave with an exec (or a Windows child process) only when
// the application hands them out again.
void mark_close_on_exec(native_socket socket) {
#if defined(_WIN32)
    if (::SetHandleInformation(reinterpret_cast<HANDLE>(os_socket(socket)), HANDLE_FLAG_INHERIT, 0) == 0) {
        throw std::system_error(static_cast<int>(::GetLastError()), asio::system_category(), "failed to make an inherited listener socket non-inheritable");
    }
#else
    const int flags = ::fcntl(socket, F_GETFD);
    if (flags == -1 || ::fcntl(socket, F_SETFD, flags | FD_CLOEXEC) == -1) {
        throw std::system_error(errno, asio::system_category(), "failed to mark an inherited listener socket close-on-exec");
    }
#endif
}

void ensure_socket_kind(native_socket socket, int type, [[maybe_unused]] int protocol, const char* message) {
    if (socket_option(socket, SO_TYPE) != type) {
        throw std::invalid_argument(message);
    }
#if defined(SO_PROTOCOL)
    if (socket_option(socket, SO_PROTOCOL) != protocol) {
        throw std::invalid_argument(message);
    }
#endif
}

void ensure_ipv6_only(native_socket socket, bool ipv6, std::optional<bool> expected) {
    if (!expected) {
        return;
    }
    int value = 0;
    auto length = static_cast<socket_length>(sizeof(value));
    if (!ipv6 || ::getsockopt(os_socket(socket), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&value), &length) != 0 || (value != 0) != *expected) {
        throw std::invalid_argument("inherited listener IPV6_V6ONLY does not match its configuration");
    }
}

}  // namespace

void close_native_socket(native_socket socket) noexcept {
#if defined(_WIN32)
    if (socket != invalid_native_socket) {
        (void)::closesocket(os_socket(socket));
    }
#else
    if (socket >= 0) {
        (void)::close(socket);
    }
#endif
}

void adopt_inherited_tcp_socket(native_socket socket, const asio::ip::tcp::endpoint& endpoint, const char* message, std::optional<bool> ipv6_only) {
    ensure_socket_kind(socket, SOCK_STREAM, IPPROTO_TCP, message);
    if (socket_option(socket, SO_ACCEPTCONN).value_or(0) == 0 || !bound_to(socket, endpoint)) {
        throw std::invalid_argument(message);
    }
    ensure_ipv6_only(socket, endpoint.address().is_v6(), ipv6_only);
    mark_close_on_exec(socket);
}

void adopt_inherited_udp_socket(native_socket socket, const asio::ip::udp::endpoint& endpoint, const char* message, std::optional<bool> ipv6_only) {
    ensure_socket_kind(socket, SOCK_DGRAM, IPPROTO_UDP, message);
    if (!bound_to(socket, endpoint)) {
        throw std::invalid_argument(message);
    }
    ensure_ipv6_only(socket, endpoint.address().is_v6(), ipv6_only);
    mark_close_on_exec(socket);
}

#if !defined(_WIN32)
native_socket duplicate_native_socket(native_socket socket) {
    const int duplicate = ::dup(socket);
    if (duplicate < 0) {
        throw std::system_error(errno, asio::system_category(), "failed to duplicate a listener socket");
    }
    return duplicate;
}
#endif

inherited_listener_sockets::~inherited_listener_sockets() {
    for (const auto socket : tcp_) {
        close_native_socket(socket);
    }
    for (const auto socket : udp_) {
        close_native_socket(socket);
    }
}

http_server_listener_definition validate_run_listener(const http_server_listener_definition& listener, const http_server_options& options, std::chrono::steady_clock::time_point key_origin) {
    const auto worker_count = options.server_.worker_count_;
    if (const auto* redirect = std::get_if<http_server_listener_definition::redirect_http_to_https>(&listener.transport_)) {
        detail::ensure_non_zero_port(redirect->https_port_, "HTTP-to-HTTPS redirect requires a fixed HTTPS listen port");
    }
    if (const auto* tls = std::get_if<http_server_listener_definition::tls>(&listener.transport_)) {
        switch (tls->tcp_mode_) {
            case http_server_listener_definition::tls_tcp_mode::tls_only:
                if (tls->plaintext_redirect_) {
                    throw std::invalid_argument("a plaintext redirect requires a mixed HTTP/HTTPS listener");
                }
                break;
            case http_server_listener_definition::tls_tcp_mode::detect_plain_or_tls:
                if (tls->plaintext_redirect_) {
                    detail::ensure_non_zero_port(tls->plaintext_redirect_->https_port_, "HTTP-to-HTTPS redirect requires a fixed HTTPS listen port");
                }
                break;
            default:
                throw std::invalid_argument("invalid TLS TCP transport mode");
        }
    }
    if (listener.connection_.has_value() && !options.connection_) {
        throw std::invalid_argument("listener_dispatch::connection requires application::on_connection()");
    }
    if (listener.request_stream_ && !options.request_stream_) {
        throw std::invalid_argument("listener_dispatch::request_stream requires application::on_request_stream()");
    }
    // A QUIC connection ID names its owning worker in 16 bits.
    if (listener.http3_.has_value() && worker_count > http3_max_worker_count) {
        throw std::invalid_argument("HTTP/3 supports at most 65536 workers");
    }
#if !defined(_WIN32)
    // Worker i serves inherited socket i; listen() admitted one per service on Windows.
    if (const auto& inherited = listener.inherited_sockets_; inherited != nullptr && ((!inherited->tcp().empty() && inherited->tcp().size() != worker_count) || (!inherited->udp().empty() && inherited->udp().size() != worker_count))) {
        throw std::invalid_argument("inherited listener sockets must number one per worker");
    }
#endif
    auto validated = listener.clone();
    // One ticket key ring per TLS listener for this run, so TLS resumption
    // does not depend on which worker accepts the connection. Credentials
    // were loaded once by listen(); every worker installs them.
    if (auto* tls = std::get_if<http_server_listener_definition::tls>(&validated.transport_)) {
        tls->session_ticket_keys_ = std::make_shared<tls_session_ticket_key_ring>(key_origin);
    }
    // One QUIC secret per HTTP/3 service, shared by every worker so any
    // worker can tell which one owns a connection ID.
    if (validated.http3_.has_value()) {
        auto secret = std::make_shared<std::array<std::uint8_t, 32>>();
        if (!fill_random_bytes(std::span<unsigned char>(secret->data(), secret->size()))) {
            throw std::runtime_error("failed to generate the HTTP/3 connection ID secret");
        }
        validated.http3_->secret_ = std::move(secret);
    }
    return validated;
}

validated_http_server_configuration::validated_http_server_configuration(std::span<const http_server_listener_definition> listeners, http_server_options&& options) :
    listeners_(),
    options_(std::move(options)),
    // Every listener's key epochs start together, so all workers rotate them
    // at the same instants.
    tls_key_origin_(std::chrono::steady_clock::now()) {
    listeners_.reserve(listeners.size());
    for (const auto& listener : listeners) {
        listeners_.push_back(validate_run_listener(listener, options_, tls_key_origin_));
    }
}

[[nodiscard]] validated_http_server_configuration validate_http_server_configuration(std::span<const http_server_listener_definition> listeners, http_server_options&& options) {
    return validated_http_server_configuration(listeners, std::move(options));
}

}  // namespace ruvia::detail

namespace ruvia {
namespace {

[[nodiscard]] bool has_tls_configuration(const tls_config& config) noexcept {
    const auto& identity = config.identity_;
    return !identity.certificate_chain_file_.empty() || !identity.private_key_file_.empty() || !identity.certificate_chain_pem_.empty() || !identity.private_key_pem_.empty() || !identity.private_key_password_.empty() || config.client_certificates_.ca_file_.has_value() || config.client_certificates_.requirement_ != tls_client_certificate_requirement::optional || !config.sni_.empty() || !config.local_addresses_.empty() || config.certificate_store_ != nullptr || static_cast<bool>(config.select_certificate_) || config.default_certificate_ != 0;
}

// Validates listen_config::dispatch_ and tls_.alpn_. For a connection-dispatch
// listener, returns the ALPN offer its HTTPS service makes, in preference
// order, as an RFC 7301 §3.1 ProtocolNameList without its length prefix.
[[nodiscard]] std::optional<std::string> connection_dispatch_alpn(const listen_config& config) {
    const auto& protocols = config.tls_.alpn_;
    switch (config.dispatch_.value_or(listener_dispatch::routes)) {
        case listener_dispatch::routes:
        case listener_dispatch::request_stream:
            if (!protocols.empty()) {
                throw std::invalid_argument("tls_config::alpn_ requires listener_dispatch::connection");
            }
            return std::nullopt;
        case listener_dispatch::connection:
            break;
        default:
            throw std::invalid_argument("listener dispatch is invalid");
    }
    if (config.http3_.has_value()) {
        throw std::invalid_argument("listener_dispatch::connection serves no HTTP/3 port");
    }
    if (config.auto_https_redirect_) {
        throw std::invalid_argument("listener_dispatch::connection cannot redirect HTTP to HTTPS");
    }
    if (!config.https_.has_value() && !protocols.empty()) {
        throw std::invalid_argument("TLS config requires an HTTPS listen port");
    }
    if (protocols.empty()) {
        return std::string("\x02h2\x08http/1.1", 12);
    }
    std::string encoded;
    for (const auto& protocol : protocols) {
        if (protocol.empty() || protocol.size() > 255) {
            throw std::invalid_argument("ALPN protocol names must be 1 to 255 bytes");
        }
        encoded.push_back(static_cast<char>(protocol.size()));
        encoded.append(protocol);
    }
    return encoded;
}

void append_listen_config(std::vector<detail::http_server_listener_definition>& replacement, const listen_config& config) {
    const auto address = detail::normalize_listen_address(config.address_.view());
    if (config.ipv6_only_ && !address.is_v6()) {
        throw std::invalid_argument("IPV6_V6ONLY requires an IPv6 listen address");
    }
    if (!config.http_.has_value() && !config.https_.has_value()) {
        throw std::invalid_argument("listen config must enable HTTP, HTTPS, or both");
    }
    detail::ensure_non_zero_optional_port(config.http_, "HTTP listen port must not be zero");
    detail::ensure_non_zero_optional_port(config.https_, "HTTPS listen port must not be zero");
    const bool mixed = config.http_.has_value() && config.http_ == config.https_;
    if (config.auto_https_redirect_ && (!config.http_.has_value() || !config.https_.has_value())) {
        throw std::invalid_argument("automatic HTTPS redirect requires both HTTP and HTTPS ports");
    }

    if (!config.https_.has_value() && has_tls_configuration(config.tls_)) {
        throw std::invalid_argument("TLS config requires an HTTPS listen port");
    }
    if (config.proxy_protocol_ && config.http3_) {
        throw std::invalid_argument("PROXY protocol is not supported on HTTP/3 listeners");
    }
    const auto connection_alpn = connection_dispatch_alpn(config);
    std::optional<detail::http_server_listener_definition::http3_service> http3;
    if (config.http3_.has_value()) {
#if defined(RUVIA_ENABLE_HTTP3)
        detail::ensure_non_zero_optional_port(config.http3_, "HTTP/3 listen port must not be zero");
        if (!config.https_.has_value()) {
            throw std::invalid_argument("HTTP/3 requires an HTTPS listen port and its TLS config");
        }
        if (config.http3_alt_svc_max_age_.count() <= 0) {
            throw std::invalid_argument("HTTP/3 Alt-Svc max age must be greater than zero");
        }
        // The secret is generated per validated configuration.
        http3.emplace(detail::http_server_listener_definition::http3_service{.port_ = config.http3_->value(), .alt_svc_max_age_ = config.http3_alt_svc_max_age_, .early_data_ = config.http3_early_data_, .retry_ = config.http3_retry_, .secret_ = {}});
#else
        throw std::invalid_argument("HTTP/3 listen port requires a build with RUVIA_ENABLE_HTTP3");
#endif
    }

    const auto ensure_unique_endpoint = [&](const asio::ip::tcp::endpoint& endpoint) {
        for (const auto& existing : replacement) {
            if (existing.endpoint_ == endpoint) {
                throw std::invalid_argument("duplicate listen endpoint");
            }
        }
    };
    // UDP endpoints are a separate namespace from the TCP ones above.
    const auto ensure_unique_http3_endpoint = [&](std::uint16_t port) {
        for (const auto& existing : replacement) {
            if (existing.http3_.has_value() && existing.http3_->port_ == port && existing.endpoint_.address() == address) {
                throw std::invalid_argument("duplicate HTTP/3 listen endpoint");
            }
        }
    };

    if (config.http_.has_value() && !mixed) {
        const auto endpoint = asio::ip::tcp::endpoint(address, config.http_->value());
        ensure_unique_endpoint(endpoint);
        if (config.auto_https_redirect_) {
            replacement.emplace_back(endpoint, detail::http_server_listener_definition::redirect_http_to_https{config.https_->value()});
        } else {
            replacement.emplace_back(endpoint);
            replacement.back().request_stream_ = config.dispatch_ == listener_dispatch::request_stream;
        }
        if (connection_alpn.has_value()) {
            replacement.back().connection_.emplace(std::string_view{});
        }
        replacement.back().ipv6_only_ = config.ipv6_only_;
        replacement.back().proxy_protocol_ = config.proxy_protocol_;
    }
    if (config.https_.has_value()) {
        const auto endpoint = asio::ip::tcp::endpoint(address, config.https_->value());
        ensure_unique_endpoint(endpoint);
        if (http3.has_value()) {
            ensure_unique_http3_endpoint(http3->port_);
        }
        // Credentials load once, here; the definition keeps no identity file
        // name, in-memory PEM or password.
        auto tls = detail::load_http_server_tls(config.tls_);
        if (mixed) {
            tls.tcp_mode_ = detail::http_server_listener_definition::tls_tcp_mode::detect_plain_or_tls;
            if (config.auto_https_redirect_) {
                tls.plaintext_redirect_.emplace(detail::http_server_listener_definition::redirect_http_to_https{config.https_->value()});
            }
        }
        replacement.emplace_back(endpoint, std::move(tls), std::move(http3));
        replacement.back().ipv6_only_ = config.ipv6_only_;
        replacement.back().proxy_protocol_ = config.proxy_protocol_;
        replacement.back().request_stream_ = config.dispatch_ == listener_dispatch::request_stream;
        if (connection_alpn.has_value()) {
            replacement.back().connection_.emplace(*connection_alpn);
        }
    }
}

class tls_secret_span_cleanser final {
public:
    explicit tls_secret_span_cleanser(std::span<const listen_config> configs) noexcept :
        configs_(configs) {}

    ~tls_secret_span_cleanser() {
        for (const auto& config : configs_) {
            detail::tls_secret_cleanser cleanser(const_cast<tls_config&>(config.tls_));
        }
    }

    tls_secret_span_cleanser(const tls_secret_span_cleanser&) = delete;
    tls_secret_span_cleanser& operator=(const tls_secret_span_cleanser&) = delete;

private:
    std::span<const listen_config> configs_;
};

// Every listen_config::inherited_sockets_ handle of `configs`, in order.
template <typename visitor_type>
void visit_inherited_sockets(std::span<const listen_config> configs, const visitor_type& visit) {
    for (const auto& config : configs) {
        if (!config.inherited_sockets_.has_value()) {
            continue;
        }
        const auto& sockets = *config.inherited_sockets_;
        for (const auto* service : {&sockets.http_, &sockets.https_, &sockets.http3_}) {
            for (const auto socket : *service) {
                visit(socket);
            }
        }
    }
}

// A handle given twice would be served, and closed, twice.
void ensure_distinct_inherited_sockets(std::span<const listen_config> configs) {
    visit_inherited_sockets(configs, [configs](native_socket socket) {
        std::size_t occurrences = 0;
        visit_inherited_sockets(configs, [socket, &occurrences](native_socket other) {
            occurrences += other == socket ? 1 : 0;
        });
        if (occurrences != 1) {
            throw std::invalid_argument("a listener socket was handed over twice");
        }
    });
}

void validate_owned_inherited_sockets(std::span<const listen_config> configs) {
    ensure_distinct_inherited_sockets(configs);
    for (const auto& config : configs) {
        if (!config.inherited_sockets_) {
            continue;
        }
        const auto address = detail::normalize_listen_address(config.address_.view());
        const auto& sockets = *config.inherited_sockets_;
        const auto tcp = [&](const auto& handles, const auto& port) {
            if (!handles.empty() && !port) {
                throw std::invalid_argument("inherited TCP sockets require their service");
            }
            for (const auto socket : handles) {
                detail::adopt_inherited_tcp_socket(socket, {address, port->value()}, "inherited TCP socket does not serve its endpoint", config.ipv6_only_);
            }
        };
        tcp(sockets.http_, config.http_);
        tcp(sockets.https_, config.https_);
        if (!sockets.http3_.empty() && !config.http3_) {
            throw std::invalid_argument("inherited UDP sockets require HTTP/3");
        }
        for (const auto socket : sockets.http3_) {
            detail::adopt_inherited_udp_socket(socket, {address, config.http3_->value()}, "inherited UDP socket does not serve its endpoint", config.ipv6_only_);
        }
    }
}

[[nodiscard]] bool holds_socket(std::span<const native_socket> sockets, native_socket socket) noexcept {
    return std::ranges::find(sockets, socket) != sockets.end();
}

// A failed listen() or add_listener() closes every handle it was given that
// no definition it built owns -- those close with their definition -- each
// handle once. No run has claimed any of them yet.
void close_unowned_inherited_sockets(std::span<const listen_config> configs, std::span<const detail::http_server_listener_definition> built) noexcept {
    std::size_t position = 0;
    visit_inherited_sockets(configs, [&](native_socket socket) {
        const auto current = position++;
        std::size_t earlier = 0;
        bool repeated = false;
        visit_inherited_sockets(configs, [&](native_socket other) {
            repeated = repeated || (earlier++ < current && other == socket);
        });
        const bool owned = std::ranges::any_of(built, [socket](const auto& definition) {
            const auto& inherited = definition.inherited_sockets_;
            return inherited != nullptr && (holds_socket(inherited->tcp(), socket) || holds_socket(inherited->udp(), socket));
        });
        if (!repeated && !owned) {
            detail::close_native_socket(socket);
        }
    });
}

// Hands config.inherited_sockets_ to the definitions append_listen_config()
// built from it, once each socket proved to serve its service.
void adopt_inherited_sockets(std::span<detail::http_server_listener_definition> built, const listen_config& config) {
    if (!config.inherited_sockets_.has_value()) {
        return;
    }
    const auto& sockets = *config.inherited_sockets_;
    if (config.http_.has_value() && config.http_ == config.https_ && !sockets.http_.empty()) {
        throw std::invalid_argument("a mixed HTTP/HTTPS listener inherits its single TCP socket group in https_, not http_");
    }
    if ((!sockets.http_.empty() && !config.http_.has_value()) || (!sockets.https_.empty() && !config.https_.has_value()) || (!sockets.http3_.empty() && !config.http3_.has_value())) {
        throw std::invalid_argument("inherited listener sockets require their service's listen port");
    }
#if defined(_WIN32)
    // The acceptor thread owns one socket per service.
    for (const auto* service : {&sockets.http_, &sockets.https_, &sockets.http3_}) {
        if (service->size() > 1) {
            throw std::invalid_argument("Windows adopts one listener socket per service");
        }
    }
#endif
    for (auto& definition : built) {
        const bool https = std::holds_alternative<detail::http_server_listener_definition::tls>(definition.transport_);
        const std::span<const native_socket> tcp = https ? sockets.https_ : sockets.http_;
        const auto udp = definition.http3_.has_value() ? std::span<const native_socket>(sockets.http3_) : std::span<const native_socket>();
        if (tcp.empty() && udp.empty()) {
            continue;
        }
        for (const auto socket : tcp) {
            detail::adopt_inherited_tcp_socket(socket, definition.endpoint_, https ? "inherited HTTPS sockets must listen on the configured address and port" : "inherited HTTP sockets must listen on the configured address and port", config.ipv6_only_);
        }
        if (!udp.empty()) {
            const asio::ip::udp::endpoint endpoint(definition.endpoint_.address(), definition.http3_->port_);
            for (const auto socket : udp) {
                detail::adopt_inherited_udp_socket(socket, endpoint, "inherited HTTP/3 sockets must be UDP sockets bound to the configured address and port", config.ipv6_only_);
            }
        }
        definition.inherited_sockets_ = std::make_shared<detail::inherited_listener_sockets>(std::vector<native_socket>(tcp.begin(), tcp.end()), std::vector<native_socket>(udp.begin(), udp.end()));
    }
}

// The listener definitions `configs` describe, with their inherited sockets.
void append_listen_configs(std::vector<detail::http_server_listener_definition>& replacement, std::span<const listen_config> configs) {
    ensure_distinct_inherited_sockets(configs);
    std::size_t endpoint_count = 0;
    for (const auto& config : configs) {
        endpoint_count += config.http_.has_value() && config.http_ == config.https_ ? 1 : static_cast<std::size_t>(config.http_.has_value()) + static_cast<std::size_t>(config.https_.has_value());
    }
    replacement.reserve(replacement.size() + endpoint_count);
    for (const auto& config : configs) {
        const auto first = replacement.size();
        append_listen_config(replacement, config);
        adopt_inherited_sockets(std::span(replacement).subspan(first), config);
    }
}

}  // namespace

// add_listener() rejects an endpoint a running listener serves, as listen()
// rejects one its call names twice.
void detail::ensure_new_listener_endpoints(std::span<const http_server_listener_definition> existing, std::span<const http_server_listener_definition> added) {
    for (const auto& candidate : added) {
        for (const auto& listener : existing) {
            if (listener.endpoint_ == candidate.endpoint_) {
                throw std::invalid_argument("duplicate listen endpoint");
            }
            if (candidate.http3_.has_value() && listener.http3_.has_value() && listener.http3_->port_ == candidate.http3_->port_ && listener.endpoint_.address() == candidate.endpoint_.address()) {
                throw std::invalid_argument("duplicate HTTP/3 listen endpoint");
            }
        }
    }
}

std::vector<detail::http_server_listener_definition> detail::load_server_listeners(std::span<const listen_config> configs) {
    const tls_secret_span_cleanser secret_cleanser(configs);
    std::vector<http_server_listener_definition> listeners;
    try {
        if (configs.empty()) {
            throw std::invalid_argument("listen config list must not be empty");
        }
        append_listen_configs(listeners, configs);
    } catch (...) {
        close_unowned_inherited_sockets(configs, listeners);
        throw;
    }
    return listeners;
}

detail::owned_listen_configs::owned_listen_configs(listen_config config) {
    try {
        configs_.reserve(1);
        configs_.push_back(std::move(config));
        own_addresses();
        validate_owned_inherited_sockets(configs_);
    } catch (...) {
        close_unowned_inherited_sockets(configs_.empty() ? std::span<const listen_config>(&config, 1) : std::span<const listen_config>(configs_), {});
        const tls_secret_span_cleanser owned_secrets(configs_);
        const detail::tls_secret_cleanser input_secrets(config.tls_);
        throw;
    }
}

detail::owned_listen_configs::owned_listen_configs(std::span<const listen_config> configs) {
    try {
        configs_.reserve(configs.size());
        for (const auto& config : configs) {
            configs_.push_back(config);
        }
        own_addresses();
        validate_owned_inherited_sockets(configs_);
    } catch (...) {
        close_unowned_inherited_sockets(configs, {});
        const tls_secret_span_cleanser owned_secrets(configs_);
        throw;
    }
}

void detail::owned_listen_configs::own_addresses() {
    addresses_.reserve(configs_.size());
    for (auto& config : configs_) {
        addresses_.emplace_back(config.address_.view());
        config.address_ = addresses_.back();
    }
}

detail::owned_listen_configs::~owned_listen_configs() {
    const tls_secret_span_cleanser secrets(configs_);
    if (inherited_owned_) {
        close_unowned_inherited_sockets(configs_, {});
    }
}

std::vector<detail::http_server_listener_definition> detail::owned_listen_configs::load() {
    inherited_owned_ = false;
    return load_server_listeners(configs_);
}

bool detail::owned_listen_configs::needs_tls_preparation() const noexcept {
    return std::ranges::any_of(configs_, [](const auto& config) {
        return config.https_.has_value();
    });
}

application& application::listen(listen_config config) & {
    return listen(std::span<const listen_config>(&config, 1));
}

application& application::listen(std::span<const listen_config> configs) & {
    const tls_secret_span_cleanser secret_cleanser(configs);
    try {
        std::lock_guard lock(state_->mutex_);
        detail::ensure_app_not_running(state_->lifecycle_.active(), "cannot change listeners while app is running");
    } catch (...) {
        close_unowned_inherited_sockets(configs, {});
        throw;
    }
    auto replacement = detail::load_server_listeners(configs);
    // The replaced definitions retire with `replacement`, after the lock,
    // closing inherited sockets no run consumed.
    detail::mutate_stopped_app(*this, *state_, "cannot change listeners while app is running", [&replacement](detail::app_state& state_value) {
        state_value.listeners_.swap(replacement);
    });
    return *this;
}

application& application::listen(std::initializer_list<listen_config> configs) & {
    return listen(std::span<const listen_config>(configs.begin(), configs.size()));
}

prepared_listener::prepared_listener(std::shared_ptr<detail::prepared_listener_state> state) noexcept :
    state_(std::move(state)) {}
prepared_listener::prepared_listener(prepared_listener&&) noexcept = default;
prepared_listener& prepared_listener::operator=(prepared_listener&& other) noexcept {
    if (this != &other) {
        auto retired = std::exchange(state_, std::move(other.state_));
        if (retired) {
            retired->decide(false);
        }
    }
    return *this;
}
prepared_listener::~prepared_listener() {
    if (auto retired = std::exchange(state_, {})) {
        retired->decide(false);
    }
}
task<void> prepared_listener::activate() && {
    return detail::prepared_listener_access::finish(std::move(*this), true);
}
task<void> prepared_listener::cancel() && {
    return detail::prepared_listener_access::finish(std::move(*this), false);
}
bool prepared_listener::valid() const noexcept {
    return state_ != nullptr;
}

}  // namespace ruvia
