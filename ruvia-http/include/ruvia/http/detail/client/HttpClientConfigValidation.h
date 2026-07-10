#pragma once

#include <array>
#include <charconv>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "ruvia/http/HttpClient.h"
#include "ruvia/http/detail/PmrResource.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"

namespace ruvia::detail {

[[nodiscard]] inline bool httpClientUsesDefaultPort(const HttpClientConfig& config) noexcept {
    return (!config.tls && config.port == 80) || (config.tls && config.port == 443);
}

[[nodiscard]] inline bool isValidHttpClientConfigHost(std::string_view host) noexcept {
    if (host.empty()) {
        return false;
    }
    std::size_t colonCount = 0;
    for (const auto ch : host) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte <= 0x20 || byte == 0x7F || byte == '/' || byte == '\\' ||
            byte == '[' || byte == ']') {
            return false;
        }
        colonCount += byte == ':' ? 1 : 0;
    }
    return colonCount != 1;
}

inline void ensureHttpClientConfigHost(
    std::string_view host,
    const char* emptyMessage,
    const char* invalidMessage) {
    if (host.empty()) {
        throw std::invalid_argument(emptyMessage);
    }
    if (!isValidHttpClientConfigHost(host)) {
        throw std::invalid_argument(invalidMessage);
    }
}

inline void validateHttpClientConfig(const HttpClientConfig& config) {
    ensureHttpClientConfigHost(
        config.host,
        "http client host must not be empty",
        "http client host is invalid");
    if (config.port == 0) {
        throw std::invalid_argument("http client port must not be zero");
    }
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
        ensureHttpClientConfigHost(
            config.tlsOptions.sniHost,
            "http client tlsOptions.sniHost must not be empty",
            "http client tlsOptions.sniHost is not a valid host name");
    }
    if (config.poolSizePerWorker == 0) {
        throw std::invalid_argument("http client pool size must be greater than zero");
    }
    if (config.proxyConnectTimeout.count() < 0 ||
        config.proxyReadTimeout.count() < 0 ||
        config.proxySendTimeout.count() < 0 ||
        config.acquireTimeout.count() < 0) {
        throw std::invalid_argument("http client timeouts must not be negative");
    }
}

[[nodiscard]] inline std::pmr::string makeHttpClientHostHeader(
    const HttpClientConfig& config,
    std::pmr::memory_resource* resource) {
    validateHttpClientConfig(config);

    auto* const targetResource = httpPmrResourceOrDefault(resource);
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
