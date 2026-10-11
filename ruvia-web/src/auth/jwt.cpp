#include "ruvia/web/auth/jwt.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include "ruvia/web/detail/json/json_escape.h"

#include "auth/jwt_primitives.h"
#include "util/constant_time.h"
#include "util/pmr_owned_storage.h"

namespace ruvia {
namespace {

[[nodiscard]] std::pmr::string move_claim_text(
    std::variant<std::string, std::pmr::string>& source,
    const std::variant<std::string, std::pmr::string>& peer,
    std::pmr::memory_resource* resource) {
    auto* owned = std::get_if<std::pmr::string>(&source);
    const auto* owned_peer = std::get_if<std::pmr::string>(&peer);
    if (owned != nullptr && owned_peer != nullptr) {
        return detail::rebind_owned_storage(*owned, resource, *owned_peer);
    }
    return std::visit([resource](const auto& text) {
        return std::pmr::string(std::string_view(text), resource);
    },
        source);
}

void validate_jwt_custom_claims(std::span<const jwt_claim> claims) {
    for (std::size_t i = 0; i < claims.size(); ++i) {
        const auto name = claims[i].name();
        if (name.empty() || detail::jwt_is_reserved_claim(name)) {
            throw std::invalid_argument("JWT custom claim name is empty or reserved");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (claims[j].name() == name) {
                throw std::invalid_argument("JWT custom claim names must be unique");
            }
        }
    }
}

}  // namespace

jwt_claim::jwt_claim(const jwt_claim& other, allocator_type allocator)
    : jwt_claim(owned_tag_type{}, std::pmr::string(other.name(), allocator.resource()),
          std::pmr::string(other.value(), allocator.resource())) {}

jwt_claim::jwt_claim(jwt_claim&& other, allocator_type allocator)
    : jwt_claim(owned_tag_type{}, move_claim_text(other.name_, other.value_, allocator.resource()),
          move_claim_text(other.value_, other.name_, allocator.resource())) {
    std::visit([](auto& text) { text.clear(); }, other.name_);
    std::visit([](auto& text) { text.clear(); }, other.value_);
}

jwt_payload::jwt_payload(jwt_payload_options options)
    : issuer_(pmr_resource_or_default(options.resource_)),
      subject_(issuer_.get_allocator().resource()),
      audiences_(issuer_.get_allocator().resource()),
      id_(issuer_.get_allocator().resource()),
      claims_(issuer_.get_allocator().resource()) {}

jwt_payload::jwt_payload(const jwt_payload& other)
    : jwt_payload(other, std::pmr::get_default_resource()) {}

jwt_payload::jwt_payload(const jwt_payload& other, std::pmr::memory_resource* resource)
    : issuer_(other.issuer_, resource),
      subject_(other.subject_, resource),
      audiences_(other.audiences_, resource),
      id_(other.id_, resource),
      expires_at_(other.expires_at_),
      not_before_(other.not_before_),
      issued_at_(other.issued_at_),
      claims_(other.claims_, resource) {}

jwt_payload::jwt_payload(jwt_payload&& other, std::pmr::memory_resource* resource)
    : issuer_(detail::rebind_owned_storage(other.issuer_, resource, other.subject_, other.audiences_, other.id_, other.claims_)),
      subject_(detail::rebind_owned_storage(other.subject_, resource, other.issuer_, other.audiences_, other.id_, other.claims_)),
      audiences_(detail::rebind_owned_storage(other.audiences_, resource, other.issuer_, other.subject_, other.id_, other.claims_)),
      id_(detail::rebind_owned_storage(other.id_, resource, other.issuer_, other.subject_, other.audiences_, other.claims_)),
      expires_at_(other.expires_at_),
      not_before_(other.not_before_),
      issued_at_(other.issued_at_),
      claims_(detail::rebind_owned_storage(other.claims_, resource, other.issuer_, other.subject_, other.audiences_, other.id_)) {
    other.issuer_.clear();
    other.subject_.clear();
    other.audiences_.clear();
    other.id_.clear();
    other.expires_at_.reset();
    other.not_before_.reset();
    other.issued_at_.reset();
    other.claims_.clear();
}

void jwt_payload::swap_values(jwt_payload& other) noexcept {
    issuer_.swap(other.issuer_);
    subject_.swap(other.subject_);
    audiences_.swap(other.audiences_);
    id_.swap(other.id_);
    expires_at_.swap(other.expires_at_);
    not_before_.swap(other.not_before_);
    issued_at_.swap(other.issued_at_);
    claims_.swap(other.claims_);
}

jwt_payload& jwt_payload::operator=(const jwt_payload& other) {
    if (this != &other) {
        jwt_payload owned(other, issuer_.get_allocator().resource());
        swap_values(owned);
    }
    return *this;
}

jwt_payload& jwt_payload::operator=(jwt_payload&& other) {
    if (this != &other) {
        jwt_payload owned(std::move(other), issuer_.get_allocator().resource());
        swap_values(owned);
    }
    return *this;
}

std::string_view jwt_payload::issuer() const& noexcept {
    return issuer_;
}
std::string_view jwt_payload::subject() const& noexcept {
    return subject_;
}
std::string_view jwt_payload::audience() const& noexcept {
    return audiences_.empty() ? std::string_view{} : std::string_view(audiences_.front());
}
bool jwt_payload::has_audience(std::string_view audience) const noexcept {
    return std::ranges::find(audiences_, audience, [](const auto& value) noexcept {
        return std::string_view(value);
    }) != audiences_.end();
}
std::string_view jwt_payload::id() const& noexcept {
    return id_;
}
std::optional<std::chrono::system_clock::time_point> jwt_payload::expires_at() const noexcept {
    return expires_at_;
}
std::optional<std::chrono::system_clock::time_point> jwt_payload::not_before() const noexcept {
    return not_before_;
}
std::optional<std::chrono::system_clock::time_point> jwt_payload::issued_at() const noexcept {
    return issued_at_;
}
std::span<const jwt_claim> jwt_payload::claims() const& noexcept {
    return claims_;
}

std::optional<std::string_view> jwt_payload::claim(std::string_view name) const& noexcept {
    for (const auto& item : claims_) {
        if (item.name() == name) {
            return item.value();
        }
    }
    return std::nullopt;
}

std::pmr::string jwt_sign(const jwt_sign_options& options) {
    validate_jwt_custom_claims(options.claims_);
    if ((options.expires_in_.has_value() && options.expires_in_->count() < 0) ||
        (options.not_before_delay_.has_value() && options.not_before_delay_->count() < 0)) {
        throw std::invalid_argument("JWT signing time offsets must not be negative");
    }
    auto* resolved = pmr_resource_or_default(options.resource_);
    const auto now = std::chrono::system_clock::now();
    std::pmr::string header(resolved);
    header.append("{\"alg\":");
    detail::append_json_string<detail::json_hex_case::lower>(header, detail::jwt_algorithm_name(options.algorithm_));
    header.append(",\"typ\":\"JWT\"}");

    std::pmr::string payload(resolved);
    payload.push_back('{');
    bool first = true;
    if (!options.issuer_.empty()) {
        detail::jwt_append_json_member(payload, first, "iss", options.issuer_);
    }
    if (!options.subject_.empty()) {
        detail::jwt_append_json_member(payload, first, "sub", options.subject_);
    }
    if (!options.audience_.empty()) {
        detail::jwt_append_json_member(payload, first, "aud", options.audience_);
    }
    if (!options.id_.empty()) {
        detail::jwt_append_json_member(payload, first, "jti", options.id_);
    }
    detail::jwt_append_json_member(payload, first, "iat", detail::jwt_epoch_seconds(now));
    if (options.expires_in_.has_value()) {
        detail::jwt_append_json_member(payload, first, "exp",
            detail::jwt_epoch_seconds(detail::jwt_time_with_offset(now, *options.expires_in_)));
    }
    if (options.not_before_delay_.has_value()) {
        const auto requested = detail::jwt_time_with_offset(now, *options.not_before_delay_);
        const auto not_before_seconds = options.not_before_delay_->count() == 0
                                            ? detail::jwt_epoch_seconds(requested)
                                            : std::chrono::ceil<std::chrono::seconds>(
                                                  requested.time_since_epoch())
                                                  .count();
        detail::jwt_append_json_member(payload, first, "nbf", not_before_seconds);
    }
    for (const auto& claim : options.claims_) {
        detail::jwt_append_json_member(payload, first, claim.name(), claim.value());
    }
    payload.push_back('}');

    auto encoded_header = detail::jwt_base64_url_encode(header, resolved);
    auto encoded_payload = detail::jwt_base64_url_encode(payload, resolved);
    std::pmr::string signing_input(resolved);
    signing_input.append(encoded_header);
    signing_input.push_back('.');
    signing_input.append(encoded_payload);
    auto signature = detail::jwt_hmac_sign(options.algorithm_, options.secret_, signing_input, resolved);
    signing_input.push_back('.');
    signing_input.append(signature);
    return signing_input;
}

jwt_payload jwt_verify(const jwt_verify_options& options) {
    if (options.leeway_.count() < 0) {
        throw std::invalid_argument("JWT verification leeway must not be negative");
    }
    if (options.expiration_claim_ != jwt_expiration_claim_policy::require &&
        options.expiration_claim_ != jwt_expiration_claim_policy::allow_missing) {
        throw std::invalid_argument("JWT expiration claim policy is invalid");
    }
    auto* resolved = pmr_resource_or_default(options.resource_);
    const auto token = options.token_.view();
    const auto parts = detail::jwt_split_token(token);
    const auto expected =
        detail::jwt_hmac_sign(options.algorithm_, options.secret_, parts.signing_input_, resolved);
    if (!detail::constant_time_bytes_equal(expected, parts.signature_)) {
        throw std::runtime_error("JWT signature verification failed");
    }
    const auto header_value = detail::jwt_base64_url_decode(parts.header_, resolved);
    if (detail::jwt_parse_jose_algorithm(header_value, resolved) !=
        detail::jwt_algorithm_name(options.algorithm_)) {
        throw std::runtime_error("JWT algorithm mismatch");
    }
    const auto payload_json = detail::jwt_base64_url_decode(parts.payload_, resolved);
    auto payload_value = detail::jwt_payload_access::decode_payload_json(payload_json, resolved);
    const auto now = std::chrono::system_clock::now();
    if (options.expiration_claim_ == jwt_expiration_claim_policy::require && !payload_value.expires_at()) {
        throw std::runtime_error("JWT token is missing exp claim");
    }
    if (payload_value.expires_at() && detail::jwt_token_expired(now, *payload_value.expires_at(), options.leeway_)) {
        throw std::runtime_error("JWT token is expired");
    }
    if (payload_value.not_before() &&
        detail::jwt_token_not_yet_valid(now, *payload_value.not_before(), options.leeway_)) {
        throw std::runtime_error("JWT token is not yet valid");
    }
    if (!options.issuer_.empty() && payload_value.issuer() != options.issuer_) {
        throw std::runtime_error("JWT issuer mismatch");
    }
    if (!options.subject_.empty() && payload_value.subject() != options.subject_) {
        throw std::runtime_error("JWT subject mismatch");
    }
    if (!options.audience_.empty() && !payload_value.has_audience(options.audience_)) {
        throw std::runtime_error("JWT audience mismatch");
    }
    return payload_value;
}

jwt_payload jwt_decode_unverified(jwt_decode_unverified_options options) {
    auto* resolved = pmr_resource_or_default(options.resource_);
    const auto token = options.token_.view();
    const auto parts = detail::jwt_split_token(token);
    const auto payload_json = detail::jwt_base64_url_decode(parts.payload_, resolved);
    return detail::jwt_payload_access::decode_payload_json(payload_json, resolved);
}

std::optional<std::string_view> jwt_bearer_token(std::string_view authorization) noexcept {
    constexpr std::string_view scheme = "Bearer";
    if (authorization.size() <= scheme.size()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < scheme.size(); ++i) {
        const auto left = authorization[i] >= 'A' && authorization[i] <= 'Z' ? authorization[i] + 32
                                                                             : authorization[i];
        const auto right = scheme[i] >= 'A' && scheme[i] <= 'Z' ? scheme[i] + 32 : scheme[i];
        if (left != right) {
            return std::nullopt;
        }
    }
    if (authorization[scheme.size()] != ' ') {
        return std::nullopt;
    }
    auto token_offset = scheme.size();
    while (token_offset < authorization.size() && authorization[token_offset] == ' ') {
        ++token_offset;
    }
    if (token_offset == authorization.size()) {
        return std::nullopt;
    }
    return authorization.substr(token_offset);
}

}  // namespace ruvia
