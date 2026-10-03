#include "ruvia/web/detail/app/AppListenerOptions.h"

#include <array>
#include <charconv>
#include <filesystem>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/IpAddress.h"
#include "ruvia/core/NativePath.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/detail/server/HttpServerOptionsValidation.h"

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
    assignTlsFileNameFromNative(output, ruvia::nativePathView(path));
}

}  // namespace

asio::ip::address normalizeListenAddress(std::string_view address) {
    if (address.empty()) {
        throw std::invalid_argument("listen address must not be empty");
    }
    const auto normalized = ruvia::parseIpAddress(address);
    if (!normalized) {
        throw std::invalid_argument("listen address must be a numeric IP address");
    }
    return *normalized;
}

bool hasTlsConfiguration(const TlsConfig& config) noexcept {
    return !config.certificateChainFile.empty() || !config.privateKeyFile.empty() ||
           !config.privateKeyPassword.empty() || config.clientCertificates.verifyFile.has_value() ||
           config.clientCertificates.requirement != TlsClientCertificateRequirement::kOptional ||
           config.http3_early_data || !config.sni.empty();
}

std::pmr::string normalizeAltSvcAdvertisement(const AltSvcConfig& config,
    std::optional<std::uint16_t> activeHttp3Port, std::pmr::memory_resource* resource) {
    auto* const targetResource = pmrResourceOrDefault(resource);
    switch (config.mode) {
        case AltSvcMode::kDisabled:
            return std::pmr::string(targetResource);
        case AltSvcMode::kClear:
            return std::pmr::string("clear", targetResource);
        case AltSvcMode::kAutomatic:
            break;
        default:
            throw std::invalid_argument("Alt-Svc mode is invalid");
    }

    if (!activeHttp3Port.has_value()) {
        return std::pmr::string(targetResource);
    }
    const auto advertisedPort = config.advertisedPort.value_or(*activeHttp3Port);
    if (advertisedPort == 0) {
        throw std::invalid_argument("Alt-Svc advertised port must not be zero");
    }
    if (config.maxAge.count() < 0) {
        throw std::invalid_argument("Alt-Svc max-age must not be negative");
    }

    std::array<char, 32> portBytes{};
    const auto [portEnd, portError] =
        std::to_chars(portBytes.data(), portBytes.data() + portBytes.size(), advertisedPort);
    std::array<char, 32> maxAgeBytes{};
    const auto [maxAgeEnd, maxAgeError] = std::to_chars(
        maxAgeBytes.data(), maxAgeBytes.data() + maxAgeBytes.size(), config.maxAge.count());
    if (portError != std::errc{} || maxAgeError != std::errc{}) {
        throw std::logic_error("Alt-Svc numeric formatting failed");
    }

    std::pmr::string result(targetResource);
    result.reserve(10 + static_cast<std::size_t>(portEnd - portBytes.data()) +
                   static_cast<std::size_t>(maxAgeEnd - maxAgeBytes.data()) +
                   (config.persist ? 11 : 0));
    result.append("h3=\":");
    result.append(portBytes.data(), static_cast<std::size_t>(portEnd - portBytes.data()));
    result.append("\"; ma=");
    result.append(
        maxAgeBytes.data(), static_cast<std::size_t>(maxAgeEnd - maxAgeBytes.data()));
    if (config.persist) {
        result.append("; persist=1");
    }
    return result;
}

HttpServerListenerDefinition::Tls normalizeTlsOptions(
    const TlsConfig& config, std::pmr::memory_resource* resource) {
    auto* const targetResource = pmrResourceOrDefault(resource);
    HttpServerListenerDefinition::Tls tls(ResolvedPmrResourceTag{}, targetResource);
    assignTlsFileName(tls.identity.certificateChainFile, config.certificateChainFile);
    assignTlsFileName(tls.identity.privateKeyFile, config.privateKeyFile);
    tls.identity.privateKeyPassword = config.privateKeyPassword;
    tls.http3_early_data = config.http3_early_data;
    if (config.clientCertificates.verifyFile.has_value() ||
        config.clientCertificates.requirement != TlsClientCertificateRequirement::kOptional) {
        auto& policy = tls.clientCertificates.emplace(
            ResolvedPmrResourceTag{}, targetResource, config.clientCertificates.requirement);
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
