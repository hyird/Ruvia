#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <openssl/types.h>

#include "ruvia/core/TcpSocketOptions.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/TlsPeerVerification.h"

namespace ruvia::detail {

struct ClientTransportConfigView final {
    TlsPeerVerificationPolicy tlsPeerVerification{TlsPeerVerificationPolicy::kVerify};
    TcpNoDelayPolicy tcpNoDelay{TcpNoDelayPolicy::kEnable};
    TcpKeepAlivePolicy tcpKeepAlive{TcpKeepAlivePolicy::kEnable};
    std::string_view caFile{};
    std::string_view certificateChainFile{};
    std::string_view privateKeyFile{};
    std::string_view privateKeyPassword{};
};

class ClientTransportConfigStorage final {
public:
    ClientTransportConfigStorage(ClientTransportConfigView source, std::pmr::memory_resource* resource);
    ClientTransportConfigStorage(const ClientTransportConfigStorage& source, std::pmr::memory_resource* resource);

    [[nodiscard]] ClientTransportConfigView view() const noexcept;

private:
    ClientTransportConfigStorage(ResolvedPmrResourceTag, ClientTransportConfigView source, std::pmr::memory_resource* resource);

    TlsPeerVerificationPolicy tlsPeerVerification_;
    TcpNoDelayPolicy tcpNoDelay_;
    TcpKeepAlivePolicy tcpKeepAlive_;
    std::pmr::string caFile_;
    std::pmr::string certificateChainFile_;
    std::pmr::string privateKeyFile_;
    std::pmr::string privateKeyPassword_;
};

template <typename Config>
[[nodiscard]] ClientTransportConfigView clientTransportConfigView(const Config& config) noexcept {
    if constexpr (requires { config.transport.view(); }) {
        return config.transport.view();
    } else {
        return {
            .tlsPeerVerification = config.tlsPeerVerification,
            .tcpNoDelay = config.tcpNoDelay,
            .tcpKeepAlive = config.tcpKeepAlive,
            .caFile = config.caFile,
            .certificateChainFile = config.certificateChainFile,
            .privateKeyFile = config.privateKeyFile,
            .privateKeyPassword = config.privateKeyPassword,
        };
    }
}

enum class ClientAlpnMode : std::uint8_t {
    kHttp11,
    kHttp2,
    kNegotiate,
};

enum class ClientTlsSetupError : std::uint8_t {
    kNone,
    kResetFailed,
    kSniFailed,
    kAlpnFailed,
};

using ClientPortTextBuffer = std::array<char, std::numeric_limits<std::uint16_t>::digits10 + 1>;

void validateClientOriginHost(std::string_view host, const char* emptyMessage, const char* invalidMessage);
[[nodiscard]] bool isClientIpAddress(std::string_view host) noexcept;
[[nodiscard]] std::string_view formatClientPort(std::uint16_t port, ClientPortTextBuffer& buffer) noexcept;
[[nodiscard]] std::string_view selectedClientAlpn(SSL* ssl) noexcept;
void validateClientTransportConfig(ClientTransportConfigView config);
void configureClientTlsContext(asio::ssl::context& context, ClientTransportConfigView config);
[[nodiscard]] ClientTlsSetupError prepareClientTlsStream(asio::ssl::stream<asio::ip::tcp::socket>& stream, const std::pmr::string& host, ClientTransportConfigView config, ClientAlpnMode alpnMode);
[[nodiscard]] std::string_view clientTlsSetupErrorMessage(ClientTlsSetupError error) noexcept;

}  // namespace ruvia::detail
