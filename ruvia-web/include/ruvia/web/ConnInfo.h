#pragma once

#include <string_view>

namespace ruvia {

class Context;

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

    [[nodiscard]] constexpr bool secure() const noexcept {
        return secure_;
    }

    // The verified peer certificate subject DN for mutual TLS, or empty when
    // no client certificate was presented.
    [[nodiscard]] constexpr std::string_view clientCertificateSubject() const noexcept {
        return clientCertificateSubject_;
    }

private:
    friend ConnInfo getConnInfo(const Context& context) noexcept;

    constexpr ConnInfo(
        std::string_view remoteAddress,
        std::string_view clientCertificateSubject,
        bool secure) noexcept
        : remote_(remoteAddress),
          clientCertificateSubject_(clientCertificateSubject),
          secure_(secure) {}

    Address remote_;
    std::string_view clientCertificateSubject_;
    bool secure_{false};
};

// Hono-like adapter boundary: connection details are queried from Context,
// never from the HTTP request model.
[[nodiscard]] ConnInfo getConnInfo(const Context& context) noexcept;

}  // namespace ruvia
