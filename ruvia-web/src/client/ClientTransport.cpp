#include "client/ClientTransport.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/ssl/host_name_verification.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/ConfigValidation.h"
#include "ruvia/core/DnsHost.h"
#include "ruvia/core/TcpSocketOptions.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/HttpRequestTarget.h"

#include "tls/TlsFilePaths.h"
#include "tls/TlsPasswordScope.h"

namespace ruvia::detail {
namespace {

constexpr std::array<unsigned char, 9> kHttp11Alpn = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
constexpr std::array<unsigned char, 3> kHttp2Alpn = {2, 'h', '2'};
constexpr std::array<unsigned char, 12> kNegotiatedHttpAlpn = {
    2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

[[nodiscard]] std::span<const unsigned char> clientAlpnBytes(ClientAlpnMode mode) noexcept {
    switch (mode) {
        case ClientAlpnMode::kHttp11:
            return kHttp11Alpn;
        case ClientAlpnMode::kHttp2:
            return kHttp2Alpn;
        case ClientAlpnMode::kNegotiate:
            return kNegotiatedHttpAlpn;
    }
    std::terminate();
}

}  // namespace

bool isClientIpAddress(std::string_view host) noexcept {
    return ::ruvia::isValidHttpIpv4Literal(host) || ::ruvia::isValidHttpIpv6Literal(host);
}

std::string_view formatClientPort(std::uint16_t port, ClientPortTextBuffer& buffer) noexcept {
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), port);
    if (error != std::errc{}) {
        std::terminate();
    }
    return {buffer.data(), static_cast<std::size_t>(end - buffer.data())};
}

std::string_view selectedClientAlpn(SSL* ssl) noexcept {
    const unsigned char* selected = nullptr;
    unsigned int size = 0;
    SSL_get0_alpn_selected(ssl, &selected, &size);
    return {reinterpret_cast<const char*>(selected), size};
}

ClientTransportConfigStorage::ClientTransportConfigStorage(
    ClientTransportConfigView source, std::pmr::memory_resource* resource)
    : ClientTransportConfigStorage(
          ResolvedPmrResourceTag{}, source, pmrResourceOrDefault(resource)) {}

ClientTransportConfigStorage::ClientTransportConfigStorage(
    ResolvedPmrResourceTag, ClientTransportConfigView source, std::pmr::memory_resource* resource)
    : tlsPeerVerification_(source.tlsPeerVerification),
      tcpNoDelay_(source.tcpNoDelay),
      tcpKeepAlive_(source.tcpKeepAlive),
      caFile_(source.caFile, resource),
      certificateChainFile_(source.certificateChainFile, resource),
      privateKeyFile_(source.privateKeyFile, resource),
      privateKeyPassword_(source.privateKeyPassword, resource) {}

ClientTransportConfigStorage::ClientTransportConfigStorage(
    const ClientTransportConfigStorage& source, std::pmr::memory_resource* resource)
    : ClientTransportConfigStorage(source.view(), resource) {}

ClientTransportConfigView ClientTransportConfigStorage::view() const noexcept {
    return {
        .tlsPeerVerification = tlsPeerVerification_,
        .tcpNoDelay = tcpNoDelay_,
        .tcpKeepAlive = tcpKeepAlive_,
        .caFile = caFile_,
        .certificateChainFile = certificateChainFile_,
        .privateKeyFile = privateKeyFile_,
        .privateKeyPassword = privateKeyPassword_,
    };
}

void validateClientOriginHost(
    std::string_view host, const char* emptyMessage, const char* invalidMessage) {
    ruvia::ensureConfigHost(host, emptyMessage, invalidMessage, ruvia::kSeparatedPortHostRules);
    if (isClientIpAddress(host)) {
        return;
    }
    if (!ruvia::isValidDnsHost(host)) {
        throw std::invalid_argument(invalidMessage);
    }
}

std::pmr::string clientUriHost(std::string_view host, std::pmr::memory_resource* resource) {
    std::pmr::string wireHost(pmrResourceOrDefault(resource));
    if ((host.find(':') != std::string_view::npos)) {
        wireHost.reserve(host.size() + 2);
        wireHost.push_back('[');
        wireHost.append(host);
        wireHost.push_back(']');
    } else {
        wireHost.assign(host);
    }
    return wireHost;
}

void validateClientTransportConfig(ClientTransportConfigView config) {
    if (config.tlsPeerVerification != TlsPeerVerificationPolicy::kVerify &&
        config.tlsPeerVerification != TlsPeerVerificationPolicy::kSkipVerification) {
        throw std::invalid_argument("client TLS peer verification policy is invalid");
    }
    ruvia::validateTcpSocketPolicies(config.tcpNoDelay, config.tcpKeepAlive);
    validate_tls_file_paths({config.caFile, config.certificateChainFile, config.privateKeyFile});
    if (config.certificateChainFile.empty() != config.privateKeyFile.empty()) {
        throw std::invalid_argument(
            "client certificate chain and private key must be configured together");
    }
}

void configure_client_tls_context(SSL_CTX& context, ClientTransportConfigView config,
    client_tls_protocol protocol) {
    validateClientTransportConfig(config);
    if (protocol != client_tls_protocol::stream && protocol != client_tls_protocol::quic) {
        throw std::invalid_argument("invalid client TLS protocol");
    }
    const bool quic = protocol == client_tls_protocol::quic;
    if (SSL_CTX_set_min_proto_version(&context, quic ? TLS1_3_VERSION : TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(&context, quic ? TLS1_3_VERSION : 0) != 1) {
        throw std::runtime_error("failed to configure client TLS versions");
    }
    const bool verify = config.tlsPeerVerification == TlsPeerVerificationPolicy::kVerify;
    SSL_CTX_set_verify(&context, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
    if (verify) {
        const int loaded = config.caFile.empty()
                               ? SSL_CTX_set_default_verify_paths(&context)
                               : SSL_CTX_load_verify_file(&context, std::pmr::string(config.caFile, processResource()).c_str());
        if (loaded != 1) {
            throw std::runtime_error("failed to load client TLS trust store");
        }
    }
    if (config.certificateChainFile.empty()) {
        return;
    }
    const tls_password_scope password_scope(context, config.privateKeyPassword);
    if (SSL_CTX_use_certificate_chain_file(&context, std::pmr::string(config.certificateChainFile, processResource()).c_str()) != 1) {
        throw std::runtime_error("failed to load client TLS certificate chain");
    }
    if (SSL_CTX_use_PrivateKey_file(&context, std::pmr::string(config.privateKeyFile, processResource()).c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(&context) != 1) {
        throw std::runtime_error("failed to load or match client TLS private key");
    }
}

ClientTlsSetupError prepareClientTlsStream(asio::ssl::stream<asio::ip::tcp::socket>& stream,
    const std::pmr::string& host, ClientTransportConfigView config, ClientAlpnMode alpnMode) {
    if (SSL_clear(stream.native_handle()) != 1) {
        return ClientTlsSetupError::kResetFailed;
    }
    const bool isIpAddress = isClientIpAddress(host);
    std::array<char, 254> sniHostBuffer;
    std::string_view tlsHost = host;
    if (!isIpAddress && host.ends_with('.')) {
        tlsHost.remove_suffix(1);
        for (std::size_t i = 0; i < tlsHost.size(); ++i) {
            sniHostBuffer[i] = tlsHost[i];
        }
        sniHostBuffer[tlsHost.size()] = '\0';
        tlsHost = std::string_view(sniHostBuffer.data(), tlsHost.size());
    }
    // RFC 6066 HostName carries a DNS host_name, never an IPv4/IPv6 literal.
    // Host verification still receives IP literals so OpenSSL can validate IP
    // subjectAltName entries.
    if (!isIpAddress &&
        SSL_set_tlsext_host_name(stream.native_handle(), tlsHost.data()) != 1) {
        return ClientTlsSetupError::kSniFailed;
    }
    if (config.tlsPeerVerification == TlsPeerVerificationPolicy::kVerify) {
        stream.set_verify_callback(asio::ssl::host_name_verification(std::string(tlsHost)));
    }
    const auto protocols = clientAlpnBytes(alpnMode);
    if (SSL_set_alpn_protos(stream.native_handle(), protocols.data(),
            static_cast<unsigned int>(protocols.size())) != 0) {
        return ClientTlsSetupError::kAlpnFailed;
    }
    return ClientTlsSetupError::kNone;
}

std::string_view clientTlsSetupErrorMessage(ClientTlsSetupError error) noexcept {
    switch (error) {
        case ClientTlsSetupError::kNone:
            return {};
        case ClientTlsSetupError::kResetFailed:
            return "failed to reset TLS stream";
        case ClientTlsSetupError::kSniFailed:
            return "failed to set TLS SNI host";
        case ClientTlsSetupError::kAlpnFailed:
            return "failed to configure TLS ALPN";
    }
    std::terminate();
}

}  // namespace ruvia::detail
