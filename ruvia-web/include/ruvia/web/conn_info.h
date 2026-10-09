#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>
#include <variant>

#include "ruvia/http/http_client.h"

namespace ruvia {

class context;
namespace detail {
class context_services;
}

class plain_connection_transport final {
private:
    friend class conn_info;
    constexpr plain_connection_transport() noexcept = default;
};

class tls_connection_transport final {
public:
    // The verified peer certificate subject DN for mutual TLS, or empty when
    // no client certificate was presented.
    [[nodiscard]] constexpr std::string_view client_certificate_subject() const noexcept {
        return client_certificate_subject_;
    }

private:
    friend class conn_info;

    explicit constexpr tls_connection_transport(std::string_view client_certificate_subject) noexcept
        : client_certificate_subject_(client_certificate_subject) {}

    std::string_view client_certificate_subject_;
};

// Runtime connection metadata associated with a Web request. This information
// comes from the server adapter rather than from the HTTP message bytes, so it
// intentionally lives outside http_request and context_request.
class conn_info final {
public:
    class address_type final {
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
        friend class conn_info;

        explicit constexpr address_type(std::string_view address, std::uint16_t port = 0) noexcept
            : address_(address),
              port_(port) {}

        std::string_view address_;
        std::uint16_t port_{0};
    };

    // The peer at the other end of this socket. Behind a reverse proxy that is
    // the proxy, not the caller -- use client() for the caller.
    [[nodiscard]] constexpr address_type remote() const noexcept {
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
    [[nodiscard]] constexpr address_type client() const noexcept {
        return client_;
    }

    // Whether client()/scheme() came from a forwarding header rather than from
    // the transport. False for every request whose peer is not trusted.
    [[nodiscard]] constexpr bool via_trusted_proxy() const noexcept {
        return via_trusted_proxy_;
    }

    // https when the request reached the client over TLS -- including TLS the
    // proxy terminated and reported with X-Forwarded-Proto -- and http
    // otherwise. tls() describes only THIS hop, so a Secure cookie or an HSTS
    // header must be decided from this instead: behind a TLS-terminating proxy
    // the server's own transport is plaintext while the client's is not.
    [[nodiscard]] constexpr http_scheme scheme() const noexcept {
        return scheme_;
    }

    [[nodiscard]] constexpr const plain_connection_transport* plain() const& noexcept {
        return std::get_if<plain_connection_transport>(&transport_);
    }
    const plain_connection_transport* plain() const&& = delete;

    [[nodiscard]] constexpr const tls_connection_transport* tls() const& noexcept {
        return std::get_if<tls_connection_transport>(&transport_);
    }
    const tls_connection_transport* tls() const&& = delete;

private:
    friend class detail::context_services;

    constexpr conn_info(std::string_view remote_address, plain_connection_transport transport) noexcept
        : remote_(remote_address),
          client_(remote_address),
          transport_(transport),
          scheme_(http_scheme::http) {}

    constexpr conn_info(std::string_view remote_address, tls_connection_transport transport) noexcept
        : remote_(remote_address),
          client_(remote_address),
          transport_(transport),
          scheme_(http_scheme::https) {}

    [[nodiscard]] static constexpr conn_info plain(std::string_view remote_address) noexcept {
        return conn_info(remote_address, plain_connection_transport{});
    }

    [[nodiscard]] static constexpr conn_info tls(
        std::string_view remote_address, std::string_view client_certificate_subject) noexcept {
        return conn_info(remote_address, tls_connection_transport(client_certificate_subject));
    }

    // Applied only after the peer has been matched against the configured
    // trusted set. An empty argument leaves the transport-derived value, so a
    // proxy that sends one field but not the other does not blank the rest.
    constexpr void apply_forwarded(
        std::string_view client_address, std::string_view forwarded_scheme) noexcept {
        if (!client_address.empty()) {
            client_ = address_type(client_address);
            via_trusted_proxy_ = true;
        }
        if (forwarded_scheme == "http") {
            scheme_ = http_scheme::http;
            via_trusted_proxy_ = true;
        } else if (forwarded_scheme == "https") {
            scheme_ = http_scheme::https;
            via_trusted_proxy_ = true;
        }
    }

    constexpr void set_remote_port(std::uint16_t port) noexcept {
        remote_ = address_type(remote_.address(), port);
        if (!via_trusted_proxy_) {
            client_ = remote_;
        }
    }

    address_type remote_;
    address_type client_;
    std::variant<plain_connection_transport, tls_connection_transport> transport_;
    http_scheme scheme_;
    bool via_trusted_proxy_{false};
};

static_assert(std::is_nothrow_copy_constructible_v<conn_info>);
static_assert(std::is_nothrow_move_constructible_v<conn_info>);
static_assert(std::is_nothrow_copy_assignable_v<conn_info>);
static_assert(std::is_nothrow_move_assignable_v<conn_info>);

}  // namespace ruvia
