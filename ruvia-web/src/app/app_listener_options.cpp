#include "app/app_listener_options.h"

#include <filesystem>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <variant>

#include "ruvia/core/ip_address.h"
#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/http/http_native_path.h"

#include "server/http_server_options_validation.h"

namespace ruvia::detail {

namespace {

template <typename native_char_type>
void assign_tls_file_name_from_native(
    std::pmr::string& output, std::basic_string_view<native_char_type> native) {
    if constexpr (std::is_same_v<native_char_type, char>) {
        output.assign(native.data(), native.size());
    } else {
        const auto name = std::filesystem::path(native.begin(), native.end()).string();
        output.assign(name.data(), name.size());
    }
}

void assign_tls_file_name(std::pmr::string& output, const std::filesystem::path& path) {
    assign_tls_file_name_from_native(output, http_native_path_view(path));
}

}  // namespace

asio::ip::address normalize_listen_address(std::string_view address) {
    if (address.empty()) {
        throw std::invalid_argument("listen address must not be empty");
    }
    const auto normalized = ruvia::parse_ip_address(address);
    if ((normalized.index() != 0)) {
        throw std::invalid_argument("listen address must be a numeric IP address");
    }
    return std::get<0>(normalized);
}

bool has_tls_configuration(const tls_config& config) noexcept {
    return !config.certificate_chain_file_.empty() || !config.private_key_file_.empty() ||
           !config.private_key_password_.empty() || config.client_certificates_.verify_file_.has_value() ||
           config.client_certificates_.requirement_ != tls_client_certificate_requirement::optional ||
           !config.sni_.empty();
}

http_server_listener_definition::tls_type normalize_tls_options(
    const tls_config& config, std::pmr::memory_resource* resource) {
    auto* const target_resource = pmr_resource_or_default(resource);
    http_server_listener_definition::tls_type tls(resolved_pmr_resource_tag{}, target_resource);
    assign_tls_file_name(tls.identity_.certificate_chain_file_, config.certificate_chain_file_);
    assign_tls_file_name(tls.identity_.private_key_file_, config.private_key_file_);
    tls.identity_.private_key_password_ = config.private_key_password_;
    if (config.client_certificates_.verify_file_.has_value() ||
        config.client_certificates_.requirement_ != tls_client_certificate_requirement::optional) {
        auto& policy = tls.client_certificates_.emplace(
            resolved_pmr_resource_tag{}, target_resource, config.client_certificates_.requirement_);
        if (config.client_certificates_.verify_file_.has_value()) {
            assign_tls_file_name(policy.verify_file_, *config.client_certificates_.verify_file_);
        }
    }
    tls.sni_identities_.reserve(config.sni_.size());
    for (const auto& configured : config.sni_) {
        auto& sni = tls.sni_identities_.emplace_back(resolved_pmr_resource_tag{}, target_resource);
        sni.host_ = configured.host_;
        assign_tls_file_name(sni.identity_.certificate_chain_file_, configured.certificate_chain_file_);
        assign_tls_file_name(sni.identity_.private_key_file_, configured.private_key_file_);
        sni.identity_.private_key_password_ = configured.private_key_password_;
    }
    validate_http_server_tls_options(tls);
    return tls;
}

}  // namespace ruvia::detail
