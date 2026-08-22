#include "ruvia/web/ServerConfig.h"
#include "ruvia/core/detail/config/ConfigValidation.h"

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

TlsIdentity TlsIdentity::fromFiles(TlsIdentityFileOptions options) {
    if (options.certificateChainFile.empty()) {
        throw std::invalid_argument("TLS certificate chain file must not be empty");
    }
    if (options.privateKeyFile.empty()) {
        throw std::invalid_argument("TLS private key file must not be empty");
    }
    return TlsIdentity(std::move(options.certificateChainFile), std::move(options.privateKeyFile), std::move(options.privateKeyPassword));
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

TlsConfig& TlsConfig::addSniIdentity(TlsSniIdentityOptions options) {
    detail::ensureSniHost(options.host, "SNI host must not be empty", "SNI host is invalid");
    for (const auto& configured : sniIdentities_) {
        if (asciiEqualsIgnoreCase(configured.host(), options.host)) {
            throw std::invalid_argument("SNI hosts must be unique");
        }
    }
    sniIdentities_.push_back(TlsSniIdentity(std::move(options.host), std::move(options.identity)));
    return *this;
}

ListenerConfig ListenerConfig::http(ListenerId id, HttpListenerOptions options) {
    if (id.value() == 0) {
        throw std::invalid_argument("listener ID must not be zero");
    }
    if (options.address.empty()) {
        throw std::invalid_argument("HTTP listen address must not be empty");
    }
    detail::ensureNonZeroPort(options.port, "HTTP listen port must not be zero");
    return ListenerConfig(id, Http{std::move(options.address), options.port});
}

ListenerConfig ListenerConfig::https(ListenerId id, HttpsListenerOptions options) {
    if (id.value() == 0) {
        throw std::invalid_argument("listener ID must not be zero");
    }
    if (options.address.empty()) {
        throw std::invalid_argument("HTTPS listen address must not be empty");
    }
    detail::ensureNonZeroPort(options.port, "HTTPS listen port must not be zero");
    return ListenerConfig(id, Https{std::move(options.address), options.port, std::move(options.tls)});
}

ListenerConfig ListenerConfig::redirectHttpToHttps(ListenerId id, RedirectHttpToHttpsListenerOptions options) {
    if (id.value() == 0 || options.target.value() == 0) {
        throw std::invalid_argument("listener ID must not be zero");
    }
    if (options.address.empty()) {
        throw std::invalid_argument("HTTP redirect listen address must not be empty");
    }
    detail::ensureNonZeroPort(options.port, "HTTP redirect listen port must not be zero");
    if (id == options.target) {
        throw std::invalid_argument("HTTP redirect and HTTPS target listeners must be different");
    }
    return ListenerConfig(id, RedirectHttpToHttps{std::move(options.address), options.port, options.target});
}

}  // namespace ruvia
