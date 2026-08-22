#include "ruvia/web/detail/app/AppListenerOptions.h"

#include <filesystem>
#include <memory_resource>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/detail/util/NativePath.h"
#include "ruvia/web/StaticFiles.h"

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

HttpServerListenerDefinition::Tls makeTlsOptions(const TlsConfig& config, std::pmr::memory_resource* resource) {
    HttpServerListenerDefinition::Tls tls(resource);
    assignTlsFileName(tls.identity.certificateChainFile, config.identity().certificateChainFile());
    assignTlsFileName(tls.identity.privateKeyFile, config.identity().privateKeyFile());
    tls.identity.privateKeyPassword = config.identity().privateKeyPassword();
    if (config.clientCertificatePolicy().has_value()) {
        auto& policy = tls.clientCertificates.emplace(resource, config.clientCertificatePolicy()->requirement());
        assignTlsFileName(policy.verifyFile, config.clientCertificatePolicy()->verifyFile());
    }
    tls.sniIdentities.reserve(config.sniIdentities().size());
    for (const auto& configured : config.sniIdentities()) {
        auto& sni = tls.sniIdentities.emplace_back(resource);
        sni.host = configured.host();
        assignTlsFileName(sni.identity.certificateChainFile, configured.identity().certificateChainFile());
        assignTlsFileName(sni.identity.privateKeyFile, configured.identity().privateKeyFile());
        sni.identity.privateKeyPassword = configured.identity().privateKeyPassword();
    }
    return tls;
}

}  // namespace ruvia::detail
