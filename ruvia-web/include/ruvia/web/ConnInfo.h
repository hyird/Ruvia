#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>
#include <variant>

namespace ruvia {

class Context;
namespace detail {
class ContextServices;
}

class PlainConnectionTransport final {
private:
    friend class ConnInfo;
    constexpr PlainConnectionTransport() noexcept = default;
};

class TlsConnectionTransport final {
public:
    // The verified peer certificate subject DN for mutual TLS, or empty when
    // no client certificate was presented.
    [[nodiscard]] constexpr std::string_view clientCertificateSubject() const noexcept {
        return clientCertificateSubject_;
    }

private:
    friend class ConnInfo;

    explicit constexpr TlsConnectionTransport(std::string_view clientCertificateSubject) noexcept
        : clientCertificateSubject_(clientCertificateSubject) {}

    std::string_view clientCertificateSubject_;
};

// Runtime connection metadata associated with a Web request. This information
// comes from the server adapter rather than from the HTTP message bytes, so it
// intentionally lives outside HttpRequest and ContextRequest.
class ConnInfo final {
public:
    class Address final {
    public:
        [[nodiscard]] constexpr std::string_view address() const noexcept {
            return address_;
        }

        // Zero when the port is not known: a forwarded client address whose
        // proxy did not record one, or a hand-built test connection.
        [[nodiscard]] constexpr std::uint16_t port() const noexcept {
            return port_;
        }

    private:
        friend class ConnInfo;

        explicit constexpr Address(std::string_view address, std::uint16_t port = 0) noexcept
            : address_(address),
              port_(port) {}

        std::string_view address_;
        std::uint16_t port_{0};
    };

    // The peer at the other end of this socket. Behind a reverse proxy that is
    // the proxy, not the caller -- use client() for the caller.
    [[nodiscard]] constexpr Address remote() const noexcept {
        return remote_;
    }

    // Who the request is from. Equal to remote() unless the peer is a configured
    // trusted proxy AND it sent a forwarding header, in which case it is what
    // that header names. Never derived from an untrusted peer's headers: those
    // are attacker-controlled, so with no trusted proxy configured this is
    // always the direct peer.
    //
    // This, not remote(), is what rate limiting keys on and what an access log
    // should record.
    [[nodiscard]] constexpr Address client() const noexcept {
        return client_;
    }

    // Whether client()/scheme() came from a forwarding header rather than from
    // the transport. False for every request whose peer is not trusted.
    [[nodiscard]] constexpr bool viaTrustedProxy() const noexcept {
        return viaTrustedProxy_;
    }

    // "https" when the request reached the client over TLS -- including TLS the
    // proxy terminated and reported with X-Forwarded-Proto -- and "http"
    // otherwise. tls() describes only THIS hop, so a Secure cookie or an HSTS
    // header must be decided from this instead: behind a TLS-terminating proxy
    // the server's own transport is plaintext while the client's is not.
    [[nodiscard]] constexpr std::string_view scheme() const noexcept {
        return scheme_;
    }

    [[nodiscard]] constexpr bool secure() const noexcept {
        return scheme_ == "https";
    }

    [[nodiscard]] constexpr const PlainConnectionTransport* plain() const& noexcept {
        return std::get_if<PlainConnectionTransport>(&transport_);
    }
    const PlainConnectionTransport* plain() const&& = delete;

    [[nodiscard]] constexpr const TlsConnectionTransport* tls() const& noexcept {
        return std::get_if<TlsConnectionTransport>(&transport_);
    }
    const TlsConnectionTransport* tls() const&& = delete;

private:
    friend class detail::ContextServices;
    friend ConnInfo getConnInfo(const Context& context) noexcept;

    constexpr ConnInfo(std::string_view remoteAddress, PlainConnectionTransport transport) noexcept
        : remote_(remoteAddress),
          client_(remoteAddress),
          transport_(transport),
          scheme_("http") {}

    constexpr ConnInfo(std::string_view remoteAddress, TlsConnectionTransport transport) noexcept
        : remote_(remoteAddress),
          client_(remoteAddress),
          transport_(transport),
          scheme_("https") {}

    [[nodiscard]] static constexpr ConnInfo plain(std::string_view remoteAddress) noexcept {
        return ConnInfo(remoteAddress, PlainConnectionTransport{});
    }

    [[nodiscard]] static constexpr ConnInfo tls(std::string_view remoteAddress, std::string_view clientCertificateSubject) noexcept {
        return ConnInfo(remoteAddress, TlsConnectionTransport(clientCertificateSubject));
    }

    // Applied only after the peer has been matched against the configured
    // trusted set. An empty argument leaves the transport-derived value, so a
    // proxy that sends one field but not the other does not blank the rest.
    constexpr void applyForwarded(std::string_view clientAddress, std::string_view forwardedScheme) noexcept {
        if (!clientAddress.empty()) {
            client_ = Address(clientAddress);
            viaTrustedProxy_ = true;
        }
        if (forwardedScheme == "http" || forwardedScheme == "https") {
            scheme_ = forwardedScheme;
            viaTrustedProxy_ = true;
        }
    }

    constexpr void setRemotePort(std::uint16_t port) noexcept {
        remote_ = Address(remote_.address(), port);
        if (!viaTrustedProxy_) {
            client_ = remote_;
        }
    }

    Address remote_;
    Address client_;
    std::variant<PlainConnectionTransport, TlsConnectionTransport> transport_;
    std::string_view scheme_;
    bool viaTrustedProxy_{false};
};

static_assert(std::is_nothrow_copy_constructible_v<ConnInfo>);
static_assert(std::is_nothrow_move_constructible_v<ConnInfo>);
static_assert(std::is_nothrow_copy_assignable_v<ConnInfo>);
static_assert(std::is_nothrow_move_assignable_v<ConnInfo>);

// Hono-like adapter boundary: connection details are queried from Context,
// never from the HTTP request model.
[[nodiscard]] ConnInfo getConnInfo(const Context& context) noexcept;

}  // namespace ruvia
