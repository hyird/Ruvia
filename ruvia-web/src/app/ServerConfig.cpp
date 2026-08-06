#include "ruvia/web/ServerConfig.h"
#include "ruvia/web/detail/app/ConfigValidation.h"

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

}  // namespace

TlsIdentity TlsIdentity::fromFiles(std::filesystem::path certificateChainFile, std::filesystem::path privateKeyFile, std::string_view privateKeyPassword) {
    if (certificateChainFile.empty()) {
        throw std::invalid_argument("TLS certificate chain file must not be empty");
    }
    if (privateKeyFile.empty()) {
        throw std::invalid_argument("TLS private key file must not be empty");
    }
    return TlsIdentity(std::move(certificateChainFile), std::move(privateKeyFile), std::string(privateKeyPassword));
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
    sniIdentities_.push_back(TlsSniIdentity(std::string(host), std::move(identity)));
    return *this;
}

ListenerConfig ListenerConfig::http(std::string_view address, std::uint16_t port) {
    if (address.empty()) {
        throw std::invalid_argument("HTTP listen address must not be empty");
    }
    detail::ensureNonZeroPort(port, "HTTP listen port must not be zero");
    return ListenerConfig(Http{std::string(address), port});
}

ListenerConfig ListenerConfig::https(std::string_view address, std::uint16_t port, TlsConfig tls) {
    if (address.empty()) {
        throw std::invalid_argument("HTTPS listen address must not be empty");
    }
    detail::ensureNonZeroPort(port, "HTTPS listen port must not be zero");
    return ListenerConfig(Https{std::string(address), port, std::move(tls)});
}

ListenerConfig ListenerConfig::redirectHttpToHttps(std::string_view address, std::uint16_t port, std::uint16_t targetHttpsPort) {
    if (address.empty()) {
        throw std::invalid_argument("HTTP redirect listen address must not be empty");
    }
    detail::ensureNonZeroPort(port, "HTTP redirect listen port must not be zero");
    detail::ensureNonZeroPort(targetHttpsPort, "HTTPS redirect target port must not be zero");
    if (port == targetHttpsPort) {
        throw std::invalid_argument("HTTP redirect and HTTPS target ports must be different");
    }
    return ListenerConfig(RedirectHttpToHttps{std::string(address), port, targetHttpsPort});
}

}  // namespace ruvia
