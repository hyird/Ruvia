#pragma once

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/web/ServerConfig.h"

namespace ruvia::detail {

struct HttpServerListenerDefinition final {
    struct TlsIdentity final {
        explicit TlsIdentity(std::pmr::memory_resource* resource = std::pmr::get_default_resource())
            : certificateChainFile(resource),
              privateKeyFile(resource),
              privateKeyPassword(resource) {}

        std::pmr::string certificateChainFile;
        std::pmr::string privateKeyFile;
        std::pmr::string privateKeyPassword;
    };

    struct TlsClientCertificatePolicy final {
        explicit TlsClientCertificatePolicy(
            std::pmr::memory_resource* resource = std::pmr::get_default_resource(),
            TlsClientCertificateRequirement configuredRequirement =
                TlsClientCertificateRequirement::kOptional)
            : verifyFile(resource),
              requirement(configuredRequirement) {}

        std::pmr::string verifyFile;
        TlsClientCertificateRequirement requirement{TlsClientCertificateRequirement::kOptional};
    };

    struct Tls final {
        struct SniIdentity final {
            explicit SniIdentity(
                std::pmr::memory_resource* resource = std::pmr::get_default_resource())
                : host(resource),
                  identity(resource) {}

            std::pmr::string host;
            TlsIdentity identity;
        };

        explicit Tls(std::pmr::memory_resource* resource = std::pmr::get_default_resource())
            : identity(resource),
              sniIdentities(resource) {}

        TlsIdentity identity;
        std::optional<TlsClientCertificatePolicy> clientCertificates;
        std::pmr::vector<SniIdentity> sniIdentities;
    };

    struct PlainHttp final {};

    struct RedirectHttpToHttps final {
        std::uint16_t httpsPort;
    };

    using Transport = std::variant<PlainHttp, Tls, RedirectHttpToHttps>;

    HttpServerListenerDefinition(
        asio::ip::tcp::endpoint configuredEndpoint, Transport configuredTransport = PlainHttp{})
        : endpoint(std::move(configuredEndpoint)),
          transport(std::move(configuredTransport)) {}

    asio::ip::tcp::endpoint endpoint;
    Transport transport;
};

using SniContextStore = std::pmr::vector<asio::ssl::context>;
using SniContextLookup = std::pmr::vector<std::pair<std::pmr::string, asio::ssl::context*>>;

class HttpServerListener final {
public:
    HttpServerListener(asio::io_context& ioContext, const HttpServerListenerDefinition& definition,
        std::pmr::memory_resource* resource);

    HttpServerListener(const HttpServerListener&) = delete;
    HttpServerListener& operator=(const HttpServerListener&) = delete;
    HttpServerListener(HttpServerListener&&) = delete;
    HttpServerListener& operator=(HttpServerListener&&) = delete;

    [[nodiscard]] const HttpServerListenerDefinition::Tls* tls() const& noexcept {
        return std::get_if<HttpServerListenerDefinition::Tls>(&transport);
    }
    const HttpServerListenerDefinition::Tls* tls() const&& = delete;

    [[nodiscard]] const HttpServerListenerDefinition::RedirectHttpToHttps* redirect()
        const& noexcept {
        return std::get_if<HttpServerListenerDefinition::RedirectHttpToHttps>(&transport);
    }
    const HttpServerListenerDefinition::RedirectHttpToHttps* redirect() const&& = delete;

    asio::ip::tcp::acceptor acceptor;
    asio::ip::tcp::endpoint endpoint;
    HttpServerListenerDefinition::Transport transport;
    std::optional<asio::ssl::context> tlsContext;
    SniContextStore sniContexts;
    SniContextLookup sniLookup;
};

}  // namespace ruvia::detail
