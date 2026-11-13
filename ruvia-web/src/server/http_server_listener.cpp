#include "server/http_server_listener.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>

#include <asio/error.hpp>

namespace ruvia::detail {
namespace {

// The identity set is immutable and shared, not copied.
http_server_listener_definition::tls clone_tls(
    const http_server_listener_definition::tls& source_value) {
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

http_server_listener_definition::transport_type clone_transport(
    const http_server_listener_definition::transport_type& source_value) {
    return std::visit(
        []<typename transport_type>(
            const transport_type& transport) -> http_server_listener_definition::transport_type {
            if constexpr (std::is_same_v<transport_type, http_server_listener_definition::tls>) {
                return clone_tls(transport);
            } else {
                return transport;
            }
        },
        source_value);
}

std::optional<http_server_listener_definition::connection_dispatch> clone_connection_dispatch(
    const std::optional<http_server_listener_definition::connection_dispatch>& source_value) {
    std::optional<http_server_listener_definition::connection_dispatch> result_value;
    if (source_value.has_value()) {
        result_value.emplace(source_value->alpn_);
    }
    return result_value;
}

}  // namespace

http_server_listener_definition http_server_listener_definition::clone() const {
    http_server_listener_definition result_value(
        endpoint_, clone_transport(transport_), http3_);
    result_value.connection_ = clone_connection_dispatch(connection_);
    result_value.inherited_sockets_ = inherited_sockets_;
    result_value.ipv6_only_ = ipv6_only_;
    result_value.proxy_protocol_ = proxy_protocol_;
    result_value.request_stream_ = request_stream_;
    return result_value;
}

http_server_session_config::http_server_session_config(
    const http_server_listener_definition& definition)
    : transport_(clone_transport(definition.transport_)),
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

void http_server_session_config::adopt_tls_identities(
    std::shared_ptr<const http_server_tls_identities> identities) noexcept {
    if (auto* tls = std::get_if<http_server_listener_definition::tls>(&transport_)) {
        tls->identities_.swap(identities);
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {
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
    if (::getsockname(os_socket(socket), local.data(), &length) != 0 || length <= 0 ||
        static_cast<std::size_t>(length) > local.capacity() || local.data()->sa_family != endpoint.data()->sa_family) {
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
        throw std::system_error(static_cast<int>(::GetLastError()), asio::system_category(),
            "failed to make an inherited listener socket non-inheritable");
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

}  // namespace ruvia::detail
