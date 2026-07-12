#pragma once

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
    [[nodiscard]] constexpr std::string_view clientCertificateSubject()
        const noexcept {
        return clientCertificateSubject_;
    }

private:
    friend class ConnInfo;

    explicit constexpr TlsConnectionTransport(
        std::string_view clientCertificateSubject) noexcept
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

    private:
        friend class ConnInfo;

        explicit constexpr Address(std::string_view address) noexcept
            : address_(address) {}

        std::string_view address_;
    };

    [[nodiscard]] constexpr Address remote() const noexcept {
        return remote_;
    }

    [[nodiscard]] constexpr const PlainConnectionTransport* plain()
        const & noexcept {
        return std::get_if<PlainConnectionTransport>(&transport_);
    }
    const PlainConnectionTransport* plain() const && = delete;

    [[nodiscard]] constexpr const TlsConnectionTransport* tls()
        const & noexcept {
        return std::get_if<TlsConnectionTransport>(&transport_);
    }
    const TlsConnectionTransport* tls() const && = delete;

private:
    friend class detail::ContextServices;
    friend ConnInfo getConnInfo(const Context& context) noexcept;

    constexpr ConnInfo(
        std::string_view remoteAddress,
        PlainConnectionTransport transport) noexcept
        : remote_(remoteAddress),
          transport_(transport) {}

    constexpr ConnInfo(
        std::string_view remoteAddress,
        TlsConnectionTransport transport) noexcept
        : remote_(remoteAddress),
          transport_(transport) {}

    [[nodiscard]] static constexpr ConnInfo plain(
        std::string_view remoteAddress) noexcept {
        return ConnInfo(remoteAddress, PlainConnectionTransport{});
    }

    [[nodiscard]] static constexpr ConnInfo tls(
        std::string_view remoteAddress,
        std::string_view clientCertificateSubject) noexcept {
        return ConnInfo(
            remoteAddress,
            TlsConnectionTransport(clientCertificateSubject));
    }

    Address remote_;
    std::variant<PlainConnectionTransport, TlsConnectionTransport> transport_;
};

static_assert(std::is_nothrow_copy_constructible_v<ConnInfo>);
static_assert(std::is_nothrow_move_constructible_v<ConnInfo>);
static_assert(std::is_nothrow_copy_assignable_v<ConnInfo>);
static_assert(std::is_nothrow_move_assignable_v<ConnInfo>);

// Hono-like adapter boundary: connection details are queried from Context,
// never from the HTTP request model.
[[nodiscard]] ConnInfo getConnInfo(const Context& context) noexcept;

}  // namespace ruvia
