#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

#include "borrowed_text.h"
#include "ip_address.h"

namespace ruvia {

// The transport facts every server-side connection description shares:
// conn_info, connection_event, tls_client_hello_record and server_connection
// derive from it, so connection_id(), local() and remote() read the same way
// on each. A plain value: copying it copies the facts.
class connection_endpoints {
public:
    constexpr connection_endpoints() noexcept = default;
    constexpr connection_endpoints(std::uint64_t id, ip_endpoint local, ip_endpoint remote) noexcept :
        id_(id),
        local_(local),
        remote_(remote) {}

    // Process-wide unique transport ID across workers, hosts and restarts.
    // Every request and observation on one connection shares this ID; retired
    // IDs are never reused. Never zero for a served connection; zero for a
    // test_app request, which has no transport connection.
    [[nodiscard]] constexpr std::uint64_t connection_id() const noexcept {
        return id_;
    }

    // The server address the connection reached. For HTTP/3 it is the address
    // the listener's UDP socket is bound to.
    [[nodiscard]] constexpr ip_endpoint local() const noexcept {
        return local_;
    }

    // The peer at the other end of this socket. Behind a reverse proxy that is
    // the proxy, not the caller -- use conn_info::client() for the caller.
    [[nodiscard]] constexpr ip_endpoint remote() const noexcept {
        return remote_;
    }

    friend constexpr bool operator==(const connection_endpoints&, const connection_endpoints&) noexcept = default;

private:
    std::uint64_t id_{};
    ip_endpoint local_{};
    ip_endpoint remote_{};
};

static_assert(std::is_trivially_copyable_v<connection_endpoints>);

class plain_connection_transport final {};

// Views borrow the connection's TLS session, which outlives every request and
// callback the connection serves.
class tls_connection_transport final {
public:
    constexpr tls_connection_transport(std::optional<borrowed_text> client_certificate_subject, borrowed_text alpn, borrowed_text server_name) noexcept :
        client_certificate_subject_(client_certificate_subject ? std::optional{client_certificate_subject->view()} : std::nullopt),
        alpn_(alpn.view()),
        server_name_(server_name.view()) {}

    template <typename string_type>
        requires detail::http_temporary_owning_char_string<string_type>
    tls_connection_transport(std::optional<string_type>&&, borrowed_text, borrowed_text) = delete;
    template <typename string_type>
        requires detail::http_temporary_owning_char_string<string_type>
    tls_connection_transport(const std::optional<string_type>&&, borrowed_text, borrowed_text) = delete;

    // Verified client certificate subject DN. Absence means no certificate;
    // an engaged empty value is a certificate with an empty Subject (RFC 5280
    // section 4.1.2.6), not an unauthenticated connection.
    [[nodiscard]] constexpr std::optional<std::string_view> client_certificate_subject() const noexcept {
        return client_certificate_subject_;
    }

    // The application protocol the handshake selected (RFC 7301); empty when
    // the client offered no ALPN.
    [[nodiscard]] constexpr std::string_view alpn() const noexcept {
        return alpn_;
    }

    // The host_name the client sent in server_name (RFC 6066 §3); empty when
    // it sent none.
    [[nodiscard]] constexpr std::string_view server_name() const noexcept {
        return server_name_;
    }

private:
    std::optional<std::string_view> client_certificate_subject_;
    std::string_view alpn_;
    std::string_view server_name_;
};

}  // namespace ruvia
