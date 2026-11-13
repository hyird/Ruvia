#include "server/http_server_tls_identity.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <asio/ssl/verify_mode.hpp>
#include <openssl/err.h>
#include <openssl/ocsp.h>
#include <openssl/ssl.h>

#include "ruvia/core/socket.h"
#include "ruvia/http/http_ascii.h"

#include "tls/dns_host.h"
#include "tls/tls_credential.h"
#include "tls/tls_protocol_policy.h"

namespace ruvia::detail {
namespace {

// In-memory PEM text of a tls_identity_config, borrowed while it is parsed.
struct tls_identity_pem final {
    std::string_view certificate_chain_;
    std::string_view private_key_;
};

void validate_ocsp_response(std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return;
    }
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("TLS OCSP response is too large");
    }
    ERR_clear_error();
    const auto* cursor = reinterpret_cast<const unsigned char*>(bytes.data());
    const auto* const end = cursor + bytes.size();
    const std::unique_ptr<OCSP_RESPONSE, decltype(&OCSP_RESPONSE_free)> response(
        d2i_OCSP_RESPONSE(nullptr, &cursor, static_cast<long>(bytes.size())), OCSP_RESPONSE_free);
    if (!response) {
        const auto error = ERR_peek_last_error();
        ERR_clear_error();
        if (ERR_GET_REASON(error) == ERR_R_MALLOC_FAILURE) {
            throw std::bad_alloc();
        }
        throw std::invalid_argument("TLS OCSP response must be a DER OCSPResponse");
    }
    const auto size = i2d_OCSP_RESPONSE(response.get(), nullptr);
    if (cursor != end || size <= 0 || static_cast<std::size_t>(size) != bytes.size()) {
        throw std::invalid_argument("TLS OCSP response must be a DER OCSPResponse");
    }
    // d2i also accepts BER; require the canonical DER encoding we advertise.
    std::vector<std::byte> canonical(bytes.size());
    auto* output = reinterpret_cast<unsigned char*>(canonical.data());
    if (i2d_OCSP_RESPONSE(response.get(), &output) != size ||
        !std::equal(bytes.begin(), bytes.end(), canonical.begin())) {
        throw std::invalid_argument("TLS OCSP response must be a DER OCSPResponse");
    }
}

// One identity's validated source and the password its key needs.
struct tls_identity_source final {
    std::variant<tls_identity_files, tls_identity_pem> location_;
    std::string_view private_key_password_;
    std::span<const std::byte> ocsp_response_;
};

[[nodiscard]] tls_identity_source make_tls_identity_source(
    const tls_identity_config& identity,
    std::span<const std::byte> ocsp_response = {}) {
    validate_ocsp_response(ocsp_response);
    const bool files = !identity.certificate_chain_file_.empty() || !identity.private_key_file_.empty();
    const bool pem = !identity.certificate_chain_pem_.empty() || !identity.private_key_pem_.empty();
    if (files && pem) {
        throw std::invalid_argument("TLS identity must come from PEM files or in-memory PEM, not both");
    }
    if (!pem) {
        // Both names are required: an empty one throws.
        return {.location_ = make_tls_identity_files(identity),
            .private_key_password_ = identity.private_key_password_,
            .ocsp_response_ = ocsp_response};
    }
    if (identity.certificate_chain_pem_.empty() || identity.private_key_pem_.empty()) {
        throw std::invalid_argument("in-memory TLS identity requires both certificate chain and private key PEM");
    }
    return {.location_ = tls_identity_pem{.certificate_chain_ = identity.certificate_chain_pem_,
                .private_key_ = identity.private_key_pem_},
        .private_key_password_ = identity.private_key_password_,
        .ocsp_response_ = ocsp_response};
}

[[nodiscard]] tls_certificate load_tls_identity(const tls_identity_source& source) {
    if (const auto* files = std::get_if<tls_identity_files>(&source.location_)) {
        return load_tls_certificate(*files, source.private_key_password_);
    }
    const auto& pem = std::get<tls_identity_pem>(source.location_);
    return tls_certificate::load_pem(pem.certificate_chain_, pem.private_key_, source.private_key_password_);
}

[[nodiscard]] http_server_tls_identities::identity load_fixed_tls_identity(
    const tls_identity_source& source) {
    return {load_tls_identity(source), source.ocsp_response_};
}

// RFC 6066 §3 HostName: an ASCII DNS name, without the trailing dot.
[[nodiscard]] bool is_valid_sni_host(std::string_view host) noexcept {
    return !host.empty() && !host.ends_with('.') && detail::is_valid_dns_host(host);
}

// A connection reaching an IPv4 address through a dual-stack socket reports
// its IPv4-mapped IPv6 form; both name the same local address.
[[nodiscard]] asio::ip::address canonical_local_address(const asio::ip::address& address) noexcept {
    if (address.is_v6()) {
        if (const auto v6 = address.to_v6(); v6.is_v4_mapped()) {
            return asio::ip::make_address_v4(asio::ip::v4_mapped, v6);
        }
    }
    return address;
}

[[nodiscard]] asio::ip::address make_tls_local_address(std::string_view text) {
    if (text.empty()) {
        throw std::invalid_argument("TLS local address must not be empty");
    }
    const auto parsed = ruvia::parse_ip_address(text);
    if (!parsed) {
        throw std::invalid_argument("TLS local address must be a numeric IP address");
    }
    const auto address = canonical_local_address(*parsed);
    // No connection arrives at an unspecified address.
    if (address.is_unspecified()) {
        throw std::invalid_argument("TLS local address must not be unspecified");
    }
    return address;
}

// Every identity setting of one tls_config, validated before anything is read.
class tls_identity_plan final {
public:
    tls_identity_plan(const tls_config& config);

    [[nodiscard]] std::shared_ptr<const http_server_tls_identities> load() const;

private:
    struct named_source final {
        // ASCII lowercase; a wildcard keeps the parent domain after its "*.".
        std::string name_;
        bool wildcard_;
        tls_identity_source source_;
    };

    struct addressed_source final {
        asio::ip::address address_;
        tls_identity_source source_;
    };

    std::optional<tls_identity_source> primary_;
    std::shared_ptr<tls_certificate_store> store_;
    std::shared_ptr<const tls_certificate_selector> selector_;
    tls_certificate_id default_certificate_{};
    std::vector<named_source> names_;
    std::vector<addressed_source> addresses_;
};

tls_identity_plan::tls_identity_plan(const tls_config& config) {
    if (config.certificate_store_) {
        const auto& identity = config.identity_;
        if (!identity.certificate_chain_file_.empty() || !identity.private_key_file_.empty() ||
            !identity.certificate_chain_pem_.empty() || !identity.private_key_pem_.empty() ||
            !identity.private_key_password_.empty() || !config.ocsp_response_.empty() ||
            !config.sni_.empty() || !config.local_addresses_.empty()) {
            throw std::invalid_argument("TLS certificate library cannot be combined with fixed identities");
        }
        if (!config.select_certificate_ && config.default_certificate_ == 0) {
            throw std::invalid_argument("TLS certificate library requires a selector or default certificate ID");
        }
        store_ = config.certificate_store_;
        selector_ = config.select_certificate_;
        default_certificate_ = config.default_certificate_;
        return;
    }
    if (config.select_certificate_ || config.default_certificate_ != 0) {
        throw std::invalid_argument("TLS certificate selection requires a certificate library");
    }
    primary_.emplace(make_tls_identity_source(config.identity_, config.ocsp_response_));
    names_.reserve(config.sni_.size());
    for (const auto& sni : config.sni_) {
        if (sni.host_.empty()) {
            throw std::invalid_argument("SNI host must not be empty");
        }
        // RFC 6125 §6.4.3: a wildcard is the whole left-most label.
        const bool wildcard = sni.host_.starts_with("*.");
        const auto host = std::string_view(sni.host_).substr(wildcard ? 2 : 0);
        if (!is_valid_sni_host(host)) {
            throw std::invalid_argument("SNI host is invalid");
        }
        std::string name(host);
        for (auto& byte : name) {
            byte = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(byte)));
        }
        for (const auto& existing : names_) {
            if (existing.wildcard_ == wildcard && existing.name_ == name) {
                throw std::invalid_argument("SNI hosts must be unique");
            }
        }
        names_.push_back(named_source{
            .name_ = std::move(name),
            .wildcard_ = wildcard,
            .source_ = make_tls_identity_source(sni.identity_, sni.ocsp_response_),
        });
    }
    addresses_.reserve(config.local_addresses_.size());
    for (const auto& local : config.local_addresses_) {
        const auto address = make_tls_local_address(local.address_);
        for (const auto& existing : addresses_) {
            if (existing.address_ == address) {
                throw std::invalid_argument("TLS local addresses must be unique");
            }
        }
        addresses_.push_back(addressed_source{
            .address_ = address,
            .source_ = make_tls_identity_source(local.identity_, local.ocsp_response_),
        });
    }
}

std::shared_ptr<const http_server_tls_identities> tls_identity_plan::load() const {
    if (store_) {
        auto identities = std::make_shared<http_server_tls_identities>();
        identities->certificate_store_ = store_;
        identities->select_certificate_ = selector_;
        identities->default_certificate_ = default_certificate_;
        return identities;
    }
    auto identities = std::make_shared<http_server_tls_identities>(load_fixed_tls_identity(*primary_));
    for (const auto& named : names_) {
        auto& target = named.wildcard_ ? identities->wildcard_names_ : identities->exact_names_;
        target.emplace_back(named.name_, load_fixed_tls_identity(named.source_));
    }
    identities->local_addresses_.reserve(addresses_.size());
    for (const auto& addressed : addresses_) {
        identities->local_addresses_.push_back(http_server_tls_identities::addressed_identity{
            .address_ = addressed.address_,
            .value_ = load_fixed_tls_identity(addressed.source_),
        });
    }
    return identities;
}

[[nodiscard]] asio::ssl::verify_mode http_server_tls_verify_mode(
    tls_client_certificate_requirement requirement) noexcept {
    auto mode = asio::ssl::verify_peer;
    if (requirement == tls_client_certificate_requirement::required) {
        mode |= asio::ssl::verify_fail_if_no_peer_cert;
    }
    return mode;
}

}  // namespace

tls_certificate load_server_tls_certificate(const tls_identity_config& identity) {
    return load_tls_identity(make_tls_identity_source(identity));
}

const http_server_tls_identities::identity& http_server_tls_identities::select(
    std::string_view server_name, const asio::ip::address* local_address) const noexcept {
    if (!server_name.empty()) {
        for (const auto& named : exact_names_) {
            if (http_ascii_equals_ignore_case(named.name_, server_name)) {
                return named.value_;
            }
        }
        // RFC 6125 §6.4.3: "*" stands for exactly one non-empty label.
        if (const auto dot = server_name.find('.'); dot != std::string_view::npos && dot != 0) {
            const auto parent = server_name.substr(dot + 1);
            for (const auto& named : wildcard_names_) {
                if (http_ascii_equals_ignore_case(named.name_, parent)) {
                    return named.value_;
                }
            }
        }
    }
    if (local_address != nullptr && !local_addresses_.empty()) {
        const auto address = canonical_local_address(*local_address);
        for (const auto& addressed : local_addresses_) {
            if (addressed.address_ == address) {
                return addressed.value_;
            }
        }
    }
    return *primary_;
}

http_server_listener_definition::tls load_http_server_tls(
    const tls_config& config) {
    validate_tls_record_limit(config.max_outgoing_records_);
    // Every setting is validated before any credential is read.
    const tls_identity_plan identities(config);
    const auto& client = config.client_certificates_;
    std::optional<tls_file_name> client_ca;
    if (client.ca_file_.has_value() || client.requirement_ != tls_client_certificate_requirement::optional) {
        switch (client.requirement_) {
            case tls_client_certificate_requirement::optional:
            case tls_client_certificate_requirement::required:
                break;
            default:
                throw std::invalid_argument("TLS client certificate requirement is invalid");
        }
        if (!client.ca_file_.has_value()) {
            throw std::invalid_argument("TLS client certificates require a CA bundle");
        }
        client_ca.emplace(make_tls_file_name(*client.ca_file_));
    }

    http_server_listener_definition::tls tls(identities.load());
    tls.max_outgoing_records_ = config.max_outgoing_records_;
    if (client_ca.has_value()) {
        auto trust_store = load_tls_trust_store(*client_ca);
        tls.client_certificates_.emplace(http_server_listener_definition::tls_client_certificate_policy{
            .requirement_ = client.requirement_,
            .client_ca_ = std::move(trust_store),
            .ca_file_ = std::move(*client_ca),
        });
    }
    return tls;
}

std::shared_ptr<const http_server_tls_identities> load_http_server_tls_identities(
    const tls_config& config) {
    return tls_identity_plan(config).load();
}

void ensure_same_http_server_tls_policy(const http_server_listener_definition::tls& current, const tls_config& config) {
    if (config.max_outgoing_records_ != current.max_outgoing_records_) {
        throw std::invalid_argument("replacing TLS identities must keep the listener's outgoing record limit");
    }
    const auto& client = config.client_certificates_;
    const auto& policy = current.client_certificates_;
    const bool same = policy.has_value()
                          ? client.requirement_ == policy->requirement_ && client.ca_file_.has_value() &&
                                make_tls_file_name(*client.ca_file_) == policy->ca_file_
                          : client.requirement_ == tls_client_certificate_requirement::optional &&
                                !client.ca_file_.has_value();
    if (!same) {
        throw std::invalid_argument("replacing TLS identities must keep the listener's client certificate policy");
    }
}

void configure_http_server_client_certificates(SSL_CTX& context_value,
    const std::optional<http_server_listener_definition::tls_client_certificate_policy>& client_certificates) {
    if (!client_certificates.has_value()) {
        return;
    }
    client_certificates->client_ca_.install(context_value);
    // A server that verifies peers without a session ID context turns every
    // resumption attempt into a fatal handshake error (SSL_CTX_set_session_id_context(3)).
    static constexpr unsigned char session_id_context[] = {'r', 'u', 'v', 'i', 'a'};
    ::ERR_clear_error();
    if (::SSL_CTX_set_session_id_context(&context_value, session_id_context, sizeof(session_id_context)) != 1) {
        throw_tls_credential_error("set_session_id_context");
    }
    SSL_CTX_set_verify(&context_value,
        static_cast<int>(http_server_tls_verify_mode(client_certificates->requirement_)), nullptr);
}

}  // namespace ruvia::detail
