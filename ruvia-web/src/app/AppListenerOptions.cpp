#include "ruvia/web/detail/app/AppListenerOptions.h"

#include <filesystem>
#include <memory_resource>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/detail/util/NativePath.h"

namespace ruvia::detail {

namespace {

template <typename NativeChar>
void assignTlsFileNameFromNative(
    std::pmr::string& output, std::basic_string_view<NativeChar> native) {
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

HttpServerListenerDefinition::Tls makeTlsOptions(
    const TlsConfig& config, std::pmr::memory_resource* resource) {
    HttpServerListenerDefinition::Tls tls(resource);
    assignTlsFileName(tls.identity.certificateChainFile, config.certificateChainFile);
    assignTlsFileName(tls.identity.privateKeyFile, config.privateKeyFile);
    tls.identity.privateKeyPassword = config.privateKeyPassword;
    if (config.clientCertificates.verifyFile.has_value()) {
        auto& policy =
            tls.clientCertificates.emplace(resource, config.clientCertificates.requirement);
        assignTlsFileName(policy.verifyFile, *config.clientCertificates.verifyFile);
    }
    tls.sniIdentities.reserve(config.sni.size());
    for (const auto& configured : config.sni) {
        auto& sni = tls.sniIdentities.emplace_back(resource);
        sni.host = configured.host;
        assignTlsFileName(sni.identity.certificateChainFile, configured.certificateChainFile);
        assignTlsFileName(sni.identity.privateKeyFile, configured.privateKeyFile);
        sni.identity.privateKeyPassword = configured.privateKeyPassword;
    }
    return tls;
}

}  // namespace ruvia::detail
