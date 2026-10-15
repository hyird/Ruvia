#include "app/app_listener_options.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include <openssl/crypto.h>

#include "ruvia/core/ip_address.h"

namespace ruvia::detail {

namespace {

void cleanse_password(std::string& password) noexcept {
    OPENSSL_cleanse(password.data(), password.size());
    password.clear();
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

tls_password_cleanser::~tls_password_cleanser() {
    cleanse_password(config_.private_key_password_);
    for (auto& sni : config_.sni_) {
        cleanse_password(sni.private_key_password_);
    }
}

}  // namespace ruvia::detail
