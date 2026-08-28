#include "ruvia/web/detail/app/AppListenerOptions.h"

#include <filesystem>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "ruvia/core/detail/util/NativePath.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/detail/server/HttpServerOptionsValidation.h"

namespace ruvia::detail {

namespace {

template <typename NativeChar>
void assignTlsFileNameFromNative(std::pmr::string& output, std::basic_string_view<NativeChar> native) {
    if constexpr (std::is_same_v<NativeChar, char>) {
        output.assign(native.data(), native.size());
    } else {
        const auto name = std::filesystem::path(native.begin(), native.end()).string();
        output.assign(name.data(), name.size());
    }
}

void assignTlsFileName(std::pmr::string& output, const std::filesystem::path& path) {
    assignTlsFileNameFromNative(output, nativePathView(path));
}

}  // namespace

asio::ip::address normalizeListenAddress(std::string_view address) {
    if (address.empty()) {
        throw std::invalid_argument("listen address must not be empty");
    }
    std::error_code error;
    auto normalized = asio::ip::make_address(address, error);
    if (error) {
        throw std::invalid_argument("listen address must be a numeric IP address");
    }
    return normalized;
}

bool hasTlsConfiguration(const TlsConfig& config) noexcept {
    return !config.certificateChainFile.empty() || !config.privateKeyFile.empty() || !config.privateKeyPassword.empty() || config.clientCertificates.verifyFile.has_value() || config.clientCertificates.requirement != TlsClientCertificateRequirement::kOptional || !config.sni.empty();
}

HttpServerListenerDefinition::Tls normalizeTlsOptions(const TlsConfig& config, std::pmr::memory_resource* resource) {
    auto* const targetResource = pmrResourceOrDefault(resource);
    HttpServerListenerDefinition::Tls tls(ResolvedPmrResourceTag{}, targetResource);
    assignTlsFileName(tls.identity.certificateChainFile, config.certificateChainFile);
    assignTlsFileName(tls.identity.privateKeyFile, config.privateKeyFile);
    tls.identity.privateKeyPassword = config.privateKeyPassword;
    if (config.clientCertificates.verifyFile.has_value() || config.clientCertificates.requirement != TlsClientCertificateRequirement::kOptional) {
        auto& policy = tls.clientCertificates.emplace(ResolvedPmrResourceTag{}, targetResource, config.clientCertificates.requirement);
        if (config.clientCertificates.verifyFile.has_value()) {
            assignTlsFileName(policy.verifyFile, *config.clientCertificates.verifyFile);
        }
    }
    tls.sniIdentities.reserve(config.sni.size());
    for (const auto& configured : config.sni) {
        auto& sni = tls.sniIdentities.emplace_back(ResolvedPmrResourceTag{}, targetResource);
        sni.host = configured.host;
        assignTlsFileName(sni.identity.certificateChainFile, configured.certificateChainFile);
        assignTlsFileName(sni.identity.privateKeyFile, configured.privateKeyFile);
        sni.identity.privateKeyPassword = configured.privateKeyPassword;
    }
    validateHttpServerTlsOptions(tls);
    return tls;
}

}  // namespace ruvia::detail
