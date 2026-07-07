#pragma once

#include <array>
#include <charconv>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "ruvia/http/HttpClient.h"
#include "ruvia/memory/PmrResource.h"
#include "ConfigValidation.h"
#include "../parser/HttpRequestTarget.h"

namespace ruvia::detail {

[[nodiscard]] inline bool httpClientUsesDefaultPort(const HttpClientConfig& config) noexcept {
    return (!config.tls && config.port == 80) || (config.tls && config.port == 443);
}

inline void validateHttpClientConfig(const HttpClientConfig& config) {
    ensureConfigHost(
        config.host,
        "http client host must not be empty",
        "http client host is invalid",
        kSeparatedPortHostRules);
    ensureNonZeroPort(config.port, "http client port must not be zero");
    // The hostHeader override is spliced verbatim into the HTTP/1 Host: line (and the HTTP/2
    // :authority), so validate it as a Host header (reg-name / [IPv6] with an optional port) --
    // this rejects CR/LF/NUL/control (header injection) AND non-host junk like spaces.
    if (!config.hostHeader.empty() &&
        !isValidHostHeader(std::string_view(config.hostHeader))) {
        throw std::invalid_argument("http client hostHeader is not a valid Host header value");
    }
    // The SNI host is advertised via SNI and matched against the certificate, so it must be a bare
    // host name / IP literal (no port, no brackets, no control/whitespace/NUL). An embedded NUL in
    // particular would desync the NUL-terminated OpenSSL SNI from the length-aware verifier.
    if (!config.tlsOptions.sniHost.empty()) {
        ensureConfigHost(
            config.tlsOptions.sniHost,
            "http client tlsOptions.sniHost must not be empty",
            "http client tlsOptions.sniHost is not a valid host name",
            kSeparatedPortHostRules);
    }
    ensurePositiveSize(config.poolSizePerWorker, "http client pool size must be greater than zero");
    ensureNonNegativeDurations(
        "http client timeouts must not be negative",
        config.proxyConnectTimeout,
        config.proxyReadTimeout,
        config.proxySendTimeout,
        config.acquireTimeout);
}

[[nodiscard]] inline std::pmr::string makeHttpClientHostHeader(
    const HttpClientConfig& config,
    std::pmr::memory_resource* resource) {
    validateHttpClientConfig(config);

    auto* const targetResource = pmrResourceOrDefault(resource);
    // An explicit Host override (e.g. a reverse proxy fronting a vhost behind an IP) is used
    // verbatim; otherwise the Host is derived from the connect host + non-default port.
    if (!config.hostHeader.empty()) {
        return std::pmr::string(config.hostHeader.data(), config.hostHeader.size(), targetResource);
    }
    const auto host = std::string_view(config.host);
    const bool needsBrackets = host.find(':') != std::string_view::npos;
    const bool includePort = !httpClientUsesDefaultPort(config);
    std::pmr::string header(targetResource);
    header.reserve(
        host.size() +
        (needsBrackets ? 2 : 0) +
        (includePort ? 6 : 0));
    if (needsBrackets) {
        header.push_back('[');
    }
    header.append(host.data(), host.size());
    if (needsBrackets) {
        header.push_back(']');
    }
    if (includePort) {
        std::array<char, 5> portBuffer;
        const auto [end, ec] = std::to_chars(
            portBuffer.data(),
            portBuffer.data() + portBuffer.size(),
            config.port);
        if (ec != std::errc{}) {
            throw std::logic_error("http client: failed to format Host port");
        }
        header.push_back(':');
        header.append(portBuffer.data(), static_cast<std::size_t>(end - portBuffer.data()));
    }
    return header;
}

}  // namespace ruvia::detail
