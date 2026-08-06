#include "ruvia/web/ServerConfig.h"
#include "ruvia/web/detail/app/ConfigValidation.h"
#include "ruvia/core/memory/ProcessResource.h"

#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <utility>

// The value types an app is configured with before it runs: a TLS identity, the
// policy for client certificates, the TLS config those make up, and the listener
// topology that decides which ports exist and what each one speaks. App's own
// setters are in AppHttpConfig.cpp; nothing here touches an App.

namespace ruvia {
namespace {

[[nodiscard]] bool asciiEqualsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const auto fold = [](char value) noexcept { return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value; };
        if (fold(lhs[i]) != fold(rhs[i])) {
            return false;
        }
    }
    return true;
}

void validateDualListenerPorts(std::uint16_t httpPort, std::uint16_t httpsPort) {
    detail::ensureNonZeroPort(httpPort, "HTTP listen port must not be zero");
    detail::ensureNonZeroPort(httpsPort, "HTTPS listen port must not be zero");
    if (httpPort == httpsPort) {
        throw std::invalid_argument("HTTP and HTTPS listen ports must be different");
    }
}

}  // namespace

TlsIdentity TlsIdentity::fromFiles(std::filesystem::path certificateChainFile, std::filesystem::path privateKeyFile, std::pmr::string privateKeyPassword) {
    if (certificateChainFile.empty()) {
        throw std::invalid_argument("TLS certificate chain file must not be empty");
    }
    if (privateKeyFile.empty()) {
        throw std::invalid_argument("TLS private key file must not be empty");
    }
    return TlsIdentity(std::move(certificateChainFile), std::move(privateKeyFile), std::pmr::string(privateKeyPassword, detail::processResource()));
}

TlsClientCertificatePolicy TlsClientCertificatePolicy::optional(std::filesystem::path verifyFile) {
    if (verifyFile.empty()) {
        throw std::invalid_argument("TLS client certificate CA bundle must not be empty");
    }
    return TlsClientCertificatePolicy(std::move(verifyFile), TlsClientCertificateRequirement::kOptional);
}

TlsClientCertificatePolicy TlsClientCertificatePolicy::required(std::filesystem::path verifyFile) {
    if (verifyFile.empty()) {
        throw std::invalid_argument("TLS client certificate CA bundle must not be empty");
    }
    return TlsClientCertificatePolicy(std::move(verifyFile), TlsClientCertificateRequirement::kRequired);
}

TlsConfig& TlsConfig::setClientCertificatePolicy(TlsClientCertificatePolicy policy) {
    clientCertificatePolicy_ = std::move(policy);
    return *this;
}

TlsConfig& TlsConfig::addSniIdentity(std::string_view host, TlsIdentity identity) {
    if (host.empty()) {
        throw std::invalid_argument("SNI host must not be empty");
    }
    for (const auto& configured : sniIdentities_) {
        if (asciiEqualsIgnoreCase(configured.host(), host)) {
            throw std::invalid_argument("SNI hosts must be unique");
        }
    }
    sniIdentities_.push_back(TlsSniIdentity(std::pmr::string(host), std::move(identity)));
    return *this;
}

ServerTopology ServerTopology::http(std::uint16_t port) {
    detail::ensureNonZeroPort(port, "HTTP listen port must not be zero");
    return ServerTopology(Http{port});
}

ServerTopology ServerTopology::https(std::uint16_t port, TlsConfig tls) {
    detail::ensureNonZeroPort(port, "HTTPS listen port must not be zero");
    return ServerTopology(Https{port, std::move(tls)});
}

ServerTopology ServerTopology::httpAndHttps(std::uint16_t httpPort, std::uint16_t httpsPort, TlsConfig tls) {
    validateDualListenerPorts(httpPort, httpsPort);
    return ServerTopology(HttpAndHttps{httpPort, httpsPort, std::move(tls)});
}

ServerTopology ServerTopology::redirectHttpToHttps(std::uint16_t httpPort, std::uint16_t httpsPort, TlsConfig tls) {
    validateDualListenerPorts(httpPort, httpsPort);
    return ServerTopology(RedirectHttpToHttps{httpPort, httpsPort, std::move(tls)});
}

}  // namespace ruvia
