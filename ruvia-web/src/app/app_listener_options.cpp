#include "app/app_listener_options.h"

#include <array>
#include <charconv>
#include <filesystem>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/core/ip_address.h"
#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/native_path.h"

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
    assign_tls_file_name_from_native(output, ruvia::native_path_view(path));
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
           config.http3_early_data_ || !config.sni_.empty();
}

std::pmr::string normalize_alt_svc_advertisement(const alt_svc_config& config,
    std::optional<std::uint16_t> active_http3_port, std::pmr::memory_resource* resource) {
    auto* const target_resource = pmr_resource_or_default(resource);
    switch (config.mode_) {
        case alt_svc_mode::disabled:
            return std::pmr::string(target_resource);
        case alt_svc_mode::clear:
            return std::pmr::string("clear", target_resource);
        case alt_svc_mode::automatic:
            break;
        default:
            throw std::invalid_argument("Alt-Svc mode is invalid");
    }

    if (!active_http3_port.has_value()) {
        return std::pmr::string(target_resource);
    }
    const auto advertised_port = config.advertised_port_.value_or(*active_http3_port);
    if (advertised_port == 0) {
        throw std::invalid_argument("Alt-Svc advertised port must not be zero");
    }
    if (config.max_age_.count() < 0) {
        throw std::invalid_argument("Alt-Svc max-age must not be negative");
    }

    std::array<char, 32> port_bytes{};
    const auto [port_end, port_error] =
        std::to_chars(port_bytes.data(), port_bytes.data() + port_bytes.size(), advertised_port);
    std::array<char, 32> max_age_bytes{};
    const auto [max_age_end, max_age_error] = std::to_chars(
        max_age_bytes.data(), max_age_bytes.data() + max_age_bytes.size(), config.max_age_.count());
    if (port_error != std::errc{} || max_age_error != std::errc{}) {
        throw std::logic_error("Alt-Svc numeric formatting failed");
    }

    std::pmr::string result(target_resource);
    result.reserve(10 + static_cast<std::size_t>(port_end - port_bytes.data()) +
                   static_cast<std::size_t>(max_age_end - max_age_bytes.data()) +
                   (config.persist_ ? 11 : 0));
    result.append("h3=\":");
    result.append(port_bytes.data(), static_cast<std::size_t>(port_end - port_bytes.data()));
    result.append("\"; ma=");
    result.append(
        max_age_bytes.data(), static_cast<std::size_t>(max_age_end - max_age_bytes.data()));
    if (config.persist_) {
        result.append("; persist=1");
    }
    return result;
}

http_server_listener_definition::tls_type normalize_tls_options(
    const tls_config& config, std::pmr::memory_resource* resource) {
    auto* const target_resource = pmr_resource_or_default(resource);
    http_server_listener_definition::tls_type tls(resolved_pmr_resource_tag{}, target_resource);
    assign_tls_file_name(tls.identity_.certificate_chain_file_, config.certificate_chain_file_);
    assign_tls_file_name(tls.identity_.private_key_file_, config.private_key_file_);
    tls.identity_.private_key_password_ = config.private_key_password_;
    tls.http3_early_data_ = config.http3_early_data_;
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
