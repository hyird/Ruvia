#include "ruvia/web/auth/jwt.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <span>
#include <stdexcept>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_base64.h"
#include "ruvia/web/detail/json/json_escape.h"

#include "auth/hmac.h"
#include "auth/jwt_date.h"
#include "auth/jwt_primitives.h"
#include "util/constant_time.h"
#include "util/pmr_owned_storage.h"

namespace ruvia {

namespace detail {
namespace {

[[nodiscard]] bool jwt_is_reserved_claim(std::string_view name) noexcept {
    return name == "iss" || name == "sub" || name == "aud" || name == "exp" || name == "nbf" ||
           name == "iat" || name == "jti";
}

// Views into the caller's token.
struct jwt_token_parts final {
    std::string_view header_;
    std::string_view payload_;
    std::string_view signature_;
    std::string_view signing_input_;
};

[[nodiscard]] jwt_token_parts jwt_split_token(std::string_view token) {
    const auto first = token.find('.');
    const auto second =
        first == std::string_view::npos ? std::string_view::npos : token.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos ||
        token.find('.', second + 1) != std::string_view::npos) {
        throw jwt_error(jwt_error::code_type::malformed_token, "JWT token must have three sections");
    }
    return jwt_token_parts{token.substr(0, first), token.substr(first + 1, second - first - 1),
        token.substr(second + 1), token.substr(0, second)};
}

// Throws std::length_error when the encoded capacity cannot be represented by
// std::size_t before allocating the result.
void jwt_base64_url_encode_into(char* out, std::string_view input) noexcept {
    http_base64_encode(out,
        std::span(reinterpret_cast<const std::uint8_t*>(input.data()), input.size()),
        http_base64_charset::url_safe, http_base64_padding::forbidden);
}

struct jwt_decoded_section final {
    static constexpr std::size_t inline_capacity = 256;
    std::array<char, inline_capacity> inline_;
    std::pmr::string heap_;
    std::size_t size_{0};

    jwt_decoded_section(std::string_view input, std::pmr::memory_resource* resource)
        : heap_(resource) {
        const auto bound = http_base64_decoded_size_bound(input.size());
        char* target = inline_.data();
        if (bound > inline_capacity) {
            heap_.resize(bound);
            target = heap_.data();
        }
        const auto decoded_size = http_base64_decode(input,
            std::span(reinterpret_cast<std::uint8_t*>(target), bound),
            http_base64_charset::url_safe, http_base64_padding::forbidden);
        if (!decoded_size) {
            throw jwt_error(jwt_error::code_type::malformed_token, "JWT base64url value is invalid");
        }
        size_ = *decoded_size;
        if (!heap_.empty()) {
            heap_.resize(size_);
        }
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return heap_.empty() ? std::string_view(inline_.data(), size_) : std::string_view(heap_);
    }
};

[[nodiscard]] hmac_digest jwt_hmac_digest(jwt_algorithm algorithm) {
    switch (algorithm) {
        case jwt_algorithm::hs256:
            return hmac_digest::sha256;
        case jwt_algorithm::hs384:
            return hmac_digest::sha384;
        case jwt_algorithm::hs512:
            return hmac_digest::sha512;
    }
    throw std::invalid_argument("unsupported JWT algorithm");
}

[[nodiscard]] std::string_view jwt_algorithm_name(jwt_algorithm algorithm) {
    switch (algorithm) {
        case jwt_algorithm::hs256:
            return "HS256";
        case jwt_algorithm::hs384:
            return "HS384";
        case jwt_algorithm::hs512:
            return "HS512";
    }
    return {};
}

[[nodiscard]] std::string_view jwt_encoded_header_prefix(jwt_algorithm algorithm) {
    switch (algorithm) {
        case jwt_algorithm::hs256:
            return "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.";
        case jwt_algorithm::hs384:
            return "eyJhbGciOiJIUzM4NCIsInR5cCI6IkpXVCJ9.";
        case jwt_algorithm::hs512:
            return "eyJhbGciOiJIUzUxMiIsInR5cCI6IkpXVCJ9.";
    }
    throw std::invalid_argument("unsupported JWT algorithm");
}

void validate_jwt_hmac_secret(std::size_t digest_size, std::string_view secret) {
    if (secret.size() > hmac_max_input_size) {
        throw std::length_error("JWT secret is too large");
    }
    // RFC 7518 section 3.2: the key must be at least as large as the hash output.
    if (secret.size() < digest_size) {
        throw std::invalid_argument("JWT secret is shorter than the selected algorithm's digest");
    }
}

// Encodes the base64url signature into `out`. Throws std::invalid_argument for
// a key shorter than the selected digest and std::length_error when the secret
// or signing input exceeds OpenSSL's HMAC length parameters.
[[nodiscard]] std::string_view jwt_hmac_sign(
    jwt_algorithm algorithm, std::string_view secret, std::string_view data, char* out) {
    const auto digest = jwt_hmac_digest(algorithm);
    const auto digest_size = hmac_digest_size(digest);
    validate_jwt_hmac_secret(digest_size, secret);
    if (data.size() > hmac_max_input_size) {
        throw std::length_error("JWT signing input is too large");
    }
    std::array<std::uint8_t, hmac_max_digest_size> mac;
    if (!compute_hmac(digest, secret, data, mac)) {
        throw std::runtime_error("JWT HMAC signing failed");
    }
    const auto encoded_size = http_base64_encoded_size(digest_size, http_base64_padding::forbidden);
    jwt_base64_url_encode_into(
        out, std::string_view(reinterpret_cast<const char*>(mac.data()), digest_size));
    return std::string_view(out, encoded_size);
}

}  // namespace

}  // namespace detail

namespace {

// The payload writer replaces each ill-formed UTF-8 subpart with U+FFFD, so
// two distinct ill-formed names could serialize as one; only well-formed
// names keep the raw uniqueness check equal to the signed one.
[[nodiscard]] bool is_well_formed_utf8(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size();) {
        if (static_cast<unsigned char>(text[i]) < 0x80) {
            ++i;
            continue;
        }
        const auto sequence = detail::scan_json_utf8_sequence(text, i);
        if (!sequence.well_formed_) {
            return false;
        }
        i += sequence.length_;
    }
    return true;
}

void validate_jwt_custom_claims(std::span<const jwt_claim_view> claims) {
    for (std::size_t i = 0; i < claims.size(); ++i) {
        const auto name = claims[i].name_.view();
        if (name.empty() || detail::jwt_is_reserved_claim(name)) {
            throw std::invalid_argument("JWT custom claim name is empty or reserved");
        }
        if (!is_well_formed_utf8(name)) {
            throw std::invalid_argument("JWT custom claim name must be well-formed UTF-8");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (claims[j].name_.view() == name) {
                throw std::invalid_argument("JWT custom claim names must be unique");
            }
        }
    }
}

}  // namespace

jwt_payload::jwt_payload(std::pmr::memory_resource* resource)
    : issuer_(pmr_resource_or_default(resource)),
      subject_(issuer_.get_allocator().resource()),
      audiences_(issuer_.get_allocator().resource()),
      id_(issuer_.get_allocator().resource()),
      claims_(issuer_.get_allocator().resource()) {}

jwt_claim& jwt_claim::operator=(jwt_claim&& other) {
    if (this != &other) {
        auto* const target = name_.get_allocator().resource();
        auto new_name = detail::rebind_owned_storage(other.name_, target, other.value_);
        auto new_value = detail::rebind_owned_storage(other.value_, target, other.name_);
        name_.swap(new_name);
        value_.swap(new_value);
        other.name_.clear();
        other.value_.clear();
    }
    return *this;
}

jwt_payload& jwt_payload::operator=(jwt_payload&& other) {
    if (this == &other) {
        return *this;
    }
    auto* const target = issuer_.get_allocator().resource();
    if (other.issuer_.get_allocator().resource() == target) {
        issuer_ = std::move(other.issuer_);
        subject_ = std::move(other.subject_);
        audiences_ = std::move(other.audiences_);
        id_ = std::move(other.id_);
        expires_at_ = std::exchange(other.expires_at_, std::nullopt);
        not_before_ = std::exchange(other.not_before_, std::nullopt);
        issued_at_ = std::exchange(other.issued_at_, std::nullopt);
        claims_ = std::move(other.claims_);
        other.issuer_.clear();
        other.subject_.clear();
        other.audiences_.clear();
        other.id_.clear();
        other.claims_.clear();
        return *this;
    }
    jwt_payload rebound(target);
    rebound.issuer_.assign(other.issuer_);
    rebound.subject_.assign(other.subject_);
    rebound.audiences_.assign(other.audiences_.begin(), other.audiences_.end());
    rebound.id_.assign(other.id_);
    if (other.expires_at_) {
        rebound.expires_at_.emplace(
            detail::jwt_payload_access::numeric_date(other.expires_at_->text(), target));
    }
    if (other.not_before_) {
        rebound.not_before_.emplace(
            detail::jwt_payload_access::numeric_date(other.not_before_->text(), target));
    }
    if (other.issued_at_) {
        rebound.issued_at_.emplace(
            detail::jwt_payload_access::numeric_date(other.issued_at_->text(), target));
    }
    rebound.claims_.reserve(other.claims_.size());
    for (const auto& item : other.claims_) {
        rebound.claims_.push_back(detail::jwt_payload_access::claim(
            std::pmr::string(item.name(), target), std::pmr::string(item.value(), target)));
    }
    *this = std::move(rebound);
    other.issuer_.clear();
    other.subject_.clear();
    other.audiences_.clear();
    other.id_.clear();
    other.expires_at_.reset();
    other.not_before_.reset();
    other.issued_at_.reset();
    other.claims_.clear();
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
std::span<const std::pmr::string> jwt_payload::audiences() const& noexcept {
    return audiences_;
}
bool jwt_payload::has_audience(std::string_view audience) const noexcept {
    return std::ranges::find(audiences_, audience, [](const auto& value) noexcept {
        return std::string_view(value);
    }) != audiences_.end();
}
std::string_view jwt_payload::id() const& noexcept {
    return id_;
}
const jwt_numeric_date* jwt_payload::expires_at() const& noexcept {
    return expires_at_ ? &*expires_at_ : nullptr;
}
const jwt_numeric_date* jwt_payload::not_before() const& noexcept {
    return not_before_ ? &*not_before_ : nullptr;
}
const jwt_numeric_date* jwt_payload::issued_at() const& noexcept {
    return issued_at_ ? &*issued_at_ : nullptr;
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
    const auto digest = detail::jwt_hmac_digest(options.algorithm_);
    detail::validate_jwt_hmac_secret(detail::hmac_digest_size(digest), options.secret_);
    const auto header_prefix = detail::jwt_encoded_header_prefix(options.algorithm_);
    auto* resolved = pmr_resource_or_default(options.resource_);
    const auto now = std::chrono::system_clock::now();

    std::size_t payload_reserve = 64U + options.issuer_.size() + options.subject_.size() +
                                  options.audience_.size() + options.id_.size() +
                                  (options.expires_in_.has_value() ? 20U : 0U) +
                                  (options.not_before_delay_.has_value() ? 20U : 0U);
    for (const auto& claim : options.claims_) {
        payload_reserve += claim.name_.size() + claim.value_.size() + 8U;
    }
    detail::jwt_json_buffer payload(resolved, payload_reserve);
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
    std::array<char, 21> date_buffer;
    detail::jwt_append_json_numeric_date(payload, first, "iat",
        detail::jwt_signing_date(date_buffer, now, std::chrono::seconds{0}));
    if (options.expires_in_.has_value()) {
        detail::jwt_append_json_numeric_date(payload, first, "exp",
            detail::jwt_signing_date(date_buffer, now, *options.expires_in_));
    }
    if (options.not_before_delay_.has_value()) {
        detail::jwt_append_json_numeric_date(payload, first, "nbf",
            detail::jwt_signing_date(date_buffer, now, *options.not_before_delay_));
    }
    for (const auto& claim : options.claims_) {
        detail::jwt_append_json_member(payload, first, claim.name_.view(), claim.value_.view());
    }
    payload.push_back('}');

    const auto encoded_payload_size =
        http_base64_encoded_size(payload.size(), http_base64_padding::forbidden);
    const auto signature_size = http_base64_encoded_size(
        detail::hmac_digest_size(digest),
        http_base64_padding::forbidden);
    if (encoded_payload_size > std::numeric_limits<std::size_t>::max() - 1 - header_prefix.size() ||
        signature_size > std::numeric_limits<std::size_t>::max() -
                             (header_prefix.size() + encoded_payload_size + 1)) {
        throw std::length_error("JWT token is too large");
    }
    const auto signing_input_size = header_prefix.size() + encoded_payload_size;
    std::pmr::string token(resolved);
    token.resize(signing_input_size + 1 + signature_size);
    header_prefix.copy(token.data(), header_prefix.size());
    detail::jwt_base64_url_encode_into(token.data() + header_prefix.size(), payload.view());
    token[signing_input_size] = '.';
    (void)detail::jwt_hmac_sign(options.algorithm_, options.secret_,
        std::string_view(token.data(), signing_input_size), token.data() + signing_input_size + 1);
    return token;
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
    std::array<char, 88> expected_buf;
    const auto expected = detail::jwt_hmac_sign(
        options.algorithm_, options.secret_, parts.signing_input_, expected_buf.data());
    if (!detail::constant_time_bytes_equal(expected, parts.signature_)) {
        throw jwt_error(jwt_error::code_type::signature_mismatch, "JWT signature verification failed");
    }
    {
        const detail::jwt_decoded_section header_value(parts.header_, resolved);
        detail::jwt_validate_jose_header(
            header_value.view(), detail::jwt_algorithm_name(options.algorithm_), resolved);
    }
    const detail::jwt_decoded_section payload_json(parts.payload_, resolved);
    auto payload_value = detail::jwt_payload_access::decode_payload_json(payload_json.view(), resolved);
    const auto* expires_at = payload_value.expires_at();
    const auto* not_before = payload_value.not_before();
    if (options.expiration_claim_ == jwt_expiration_claim_policy::require && expires_at == nullptr) {
        throw jwt_error(jwt_error::code_type::missing_claim, "JWT token is missing exp claim");
    }
    if (expires_at != nullptr || not_before != nullptr) {
        const auto now = std::chrono::system_clock::now();
        // RFC 7519 §4.1.4: exp == now - leeway is already expired.
        if (expires_at != nullptr &&
            detail::jwt_compare_numeric_date(expires_at->text(), now, -options.leeway_) != std::strong_ordering::greater) {
            throw jwt_error(jwt_error::code_type::expired, "JWT token is expired");
        }
        // RFC 7519 §4.1.5: nbf == now + leeway is already valid.
        if (not_before != nullptr &&
            detail::jwt_compare_numeric_date(not_before->text(), now, options.leeway_) == std::strong_ordering::greater) {
            throw jwt_error(jwt_error::code_type::not_yet_valid, "JWT token is not yet valid");
        }
    }
    if (!options.issuer_.empty() && payload_value.issuer() != options.issuer_.view()) {
        throw jwt_error(jwt_error::code_type::claim_mismatch, "JWT issuer mismatch");
    }
    if (!options.subject_.empty() && payload_value.subject() != options.subject_.view()) {
        throw jwt_error(jwt_error::code_type::claim_mismatch, "JWT subject mismatch");
    }
    // RFC 7519 §4.1.3: a token that names audiences is rejected unless this
    // verifier identifies itself with one of them.
    if (options.audience_.empty()) {
        if (!payload_value.audiences_.empty()) {
            throw jwt_error(
                jwt_error::code_type::claim_mismatch, "JWT audience is present but none is expected");
        }
    } else if (!payload_value.has_audience(options.audience_.view())) {
        throw jwt_error(jwt_error::code_type::claim_mismatch, "JWT audience mismatch");
    }
    return payload_value;
}

jwt_payload jwt_decode_unverified(const jwt_decode_unverified_options& options) {
    auto* resolved = pmr_resource_or_default(options.resource_);
    const auto token = options.token_.view();
    const auto parts = detail::jwt_split_token(token);
    {
        const detail::jwt_decoded_section header_value(parts.header_, resolved);
        detail::jwt_validate_jose_header(header_value.view(), std::nullopt, resolved);
    }
    const detail::jwt_decoded_section payload_json(parts.payload_, resolved);
    return detail::jwt_payload_access::decode_payload_json(payload_json.view(), resolved);
}

std::optional<std::string_view> jwt_bearer_token(std::string_view authorization) noexcept {
    constexpr std::string_view scheme = "Bearer";
    if (authorization.size() <= scheme.size() ||
        !http_ascii_equals_ignore_case(authorization.substr(0, scheme.size()), scheme)) {
        return std::nullopt;
    }
    if (authorization[scheme.size()] != ' ') {
        return std::nullopt;
    }
    auto token_offset = scheme.size();
    while (token_offset < authorization.size() && authorization[token_offset] == ' ') {
        ++token_offset;
    }
    // RFC 6750 §2.1: b64token = 1*( ALPHA / DIGIT / "-" / "." / "_" / "~" /
    // "+" / "/" ) *"=".
    const auto is_b64token_char = [](char value) noexcept {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
               value == '-' || value == '.' || value == '_' || value == '~' || value == '+' || value == '/';
    };
    const auto token = authorization.substr(token_offset);
    std::size_t index = 0;
    while (index < token.size() && is_b64token_char(token[index])) {
        ++index;
    }
    if (index == 0) {
        return std::nullopt;
    }
    while (index < token.size() && token[index] == '=') {
        ++index;
    }
    if (index != token.size()) {
        return std::nullopt;
    }
    return token;
}

}  // namespace ruvia
