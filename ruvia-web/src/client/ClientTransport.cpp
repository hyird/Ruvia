#include "ruvia/web/detail/client/ClientTransport.h"

#include <array>
#include <charconv>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <asio/ip/address.hpp>
#include <asio/ssl/host_name_verification.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/detail/config/ConfigValidation.h"
#include "ruvia/core/detail/io/TcpSocketOptions.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"

namespace ruvia::detail {
namespace {

constexpr std::array<unsigned char, 9> kHttp11Alpn = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
constexpr std::array<unsigned char, 3> kHttp2Alpn = {2, 'h', '2'};
constexpr std::array<unsigned char, 12> kNegotiatedHttpAlpn = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

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
    std::error_code error;
    (void)asio::ip::make_address(host, error);
    return !error;
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

ClientTransportConfigStorage::ClientTransportConfigStorage(ClientTransportConfigView source, std::pmr::memory_resource* resource)
    : ClientTransportConfigStorage(ResolvedPmrResourceTag{}, source, pmrResourceOrDefault(resource)) {}

ClientTransportConfigStorage::ClientTransportConfigStorage(ResolvedPmrResourceTag, ClientTransportConfigView source, std::pmr::memory_resource* resource)
    : tlsPeerVerification_(source.tlsPeerVerification),
      tcpNoDelay_(source.tcpNoDelay),
      tcpKeepAlive_(source.tcpKeepAlive),
      caFile_(source.caFile, resource),
      certificateChainFile_(source.certificateChainFile, resource),
      privateKeyFile_(source.privateKeyFile, resource),
      privateKeyPassword_(source.privateKeyPassword, resource) {}

ClientTransportConfigStorage::ClientTransportConfigStorage(const ClientTransportConfigStorage& source, std::pmr::memory_resource* resource)
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

void validateClientOriginHost(std::string_view host, const char* emptyMessage, const char* invalidMessage) {
    ensureConfigHost(host, emptyMessage, invalidMessage, kSeparatedPortHostRules);
    std::string wireHost;
    if (host.find(':') != std::string_view::npos) {
        wireHost.reserve(host.size() + 2);
        wireHost.push_back('[');
        wireHost.append(host);
        wireHost.push_back(']');
    } else {
        wireHost.assign(host);
    }
    if (!isValidHttpHost(wireHost)) {
        throw std::invalid_argument(invalidMessage);
    }
}

void validateClientTransportConfig(ClientTransportConfigView config) {
    if (config.tlsPeerVerification != TlsPeerVerificationPolicy::kVerify && config.tlsPeerVerification != TlsPeerVerificationPolicy::kSkipVerification) {
        throw std::invalid_argument("client TLS peer verification policy is invalid");
    }
    validateTcpNoDelayPolicy(config.tcpNoDelay);
    validateTcpKeepAlivePolicy(config.tcpKeepAlive);
    if (config.certificateChainFile.empty() != config.privateKeyFile.empty()) {
        throw std::invalid_argument("client certificate chain and private key must be configured together");
    }
}

void configureClientTlsContext(asio::ssl::context& context, ClientTransportConfigView config) {
    if (config.tlsPeerVerification == TlsPeerVerificationPolicy::kVerify) {
        context.set_verify_mode(asio::ssl::verify_peer);
        if (config.caFile.empty()) {
            context.set_default_verify_paths();
        } else {
            context.load_verify_file(std::string(config.caFile));
        }
    } else {
        context.set_verify_mode(asio::ssl::verify_none);
    }
    if (!config.privateKeyPassword.empty()) {
        auto password = std::string(config.privateKeyPassword);
        context.set_password_callback([password = std::move(password)](std::size_t, asio::ssl::context_base::password_purpose) { return password; });
    }
    if (!config.certificateChainFile.empty()) {
        context.use_certificate_chain_file(std::string(config.certificateChainFile));
        context.use_private_key_file(std::string(config.privateKeyFile), asio::ssl::context::pem);
    }
}

ClientTlsSetupError prepareClientTlsStream(asio::ssl::stream<asio::ip::tcp::socket>& stream, const std::pmr::string& host, ClientTransportConfigView config, ClientAlpnMode alpnMode) {
    if (SSL_clear(stream.native_handle()) != 1) {
        return ClientTlsSetupError::kResetFailed;
    }
    // RFC 6066 HostName carries a DNS host_name, never an IPv4/IPv6 literal.
    // Host verification still receives IP literals so OpenSSL can validate IP
    // subjectAltName entries.
    if (!isClientIpAddress(host) && SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()) != 1) {
        return ClientTlsSetupError::kSniFailed;
    }
    if (config.tlsPeerVerification == TlsPeerVerificationPolicy::kVerify) {
        stream.set_verify_callback(asio::ssl::host_name_verification(std::string(host)));
    }
    const auto protocols = clientAlpnBytes(alpnMode);
    if (SSL_set_alpn_protos(stream.native_handle(), protocols.data(), static_cast<unsigned int>(protocols.size())) != 0) {
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
