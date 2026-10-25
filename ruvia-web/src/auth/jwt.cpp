#include "ruvia/web/auth/jwt.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <ratio>
#include <span>
#include <stdexcept>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_base64.h"
#include "ruvia/web/detail/json/json_escape.h"

#include "auth/hmac.h"
#include "auth/jwt_primitives.h"
#include "util/constant_time.h"

namespace ruvia {

namespace detail {
namespace {

// Split without subtracting a rounded time_point: floor(seconds) at the
// clock's minimum can itself lie below the representable clock range.
using jwt_clock_second_ratio_type = std::ratio_divide<std::chrono::seconds::period,
    std::chrono::system_clock::period>;
static_assert(jwt_clock_second_ratio_type::den == 1 && jwt_clock_second_ratio_type::num > 1,
    "JWT time arithmetic requires an integral subsecond system clock");
static_assert(std::numeric_limits<std::chrono::system_clock::rep>::is_integer &&
                  std::numeric_limits<std::chrono::system_clock::rep>::is_signed &&
                  std::numeric_limits<std::chrono::system_clock::rep>::digits <=
                      std::numeric_limits<std::chrono::seconds::rep>::digits,
    "JWT whole-second distances must fit in the signed seconds representation");

struct jwt_clock_parts final {
    std::chrono::seconds whole_seconds_{};
    std::chrono::system_clock::duration fraction_{};
};

[[nodiscard]] constexpr jwt_clock_parts jwt_split_clock_time(std::chrono::system_clock::time_point value) noexcept {
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch());
    auto remainder = value.time_since_epoch().count() % jwt_clock_second_ratio_type::num;
    if (remainder < 0) {
        seconds -= std::chrono::seconds{1};
        remainder += jwt_clock_second_ratio_type::num;
    }
    return {seconds, std::chrono::system_clock::duration{remainder}};
}

[[nodiscard]] std::chrono::system_clock::time_point jwt_time_with_offset(
    std::chrono::system_clock::time_point value, std::chrono::seconds offset) noexcept {
    using clock_type = std::chrono::system_clock;
    const auto parts = jwt_split_clock_time(value);
    constexpr auto minimum = jwt_split_clock_time(clock_type::time_point::min());
    constexpr auto maximum = jwt_split_clock_time(clock_type::time_point::max());
    if (offset > maximum.whole_seconds_ - parts.whole_seconds_) {
        return clock_type::time_point::max();
    }
    if (offset < minimum.whole_seconds_ - parts.whole_seconds_) {
        return clock_type::time_point::min();
    }
    const auto target = parts.whole_seconds_ + offset;
    if (target == minimum.whole_seconds_) {
        if (parts.fraction_ < minimum.fraction_) {
            return clock_type::time_point::min();
        }
        // Do not convert the minimum's floored seconds back into clock ticks:
        // that intermediate can underflow even when the final value fits.
        return clock_type::time_point::min() + (parts.fraction_ - minimum.fraction_);
    }
    if (target == maximum.whole_seconds_ && parts.fraction_ > maximum.fraction_) {
        return clock_type::time_point::max();
    }
    return clock_type::time_point(std::chrono::duration_cast<clock_type::duration>(target) + parts.fraction_);
}

[[nodiscard]] std::int64_t jwt_epoch_seconds(std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
}

// RFC 7519 §4.1.4: a token is valid only while the current time is *before*
// "exp", so at now == exp (no leeway) it MUST be rejected. leeway widens the
// accepted window past exp.
[[nodiscard]] bool jwt_token_expired(jwt_clock_parts now_parts,
    std::chrono::system_clock::time_point expires_at, std::chrono::seconds leeway) noexcept {
    const auto expires_parts = jwt_split_clock_time(expires_at);
    // Subsecond clock resolution keeps the whole-second distance representable
    // even when subtracting time_points directly would overflow their duration.
    const auto distance = now_parts.whole_seconds_ - expires_parts.whole_seconds_;
    return distance > leeway ||
           (distance == leeway && now_parts.fraction_ >= expires_parts.fraction_);
}

// RFC 7519 §4.1.5: a token is valid only when the current time is *after or
// equal to* "nbf"; leeway widens the accepted window earlier. Rejected while
// now (plus leeway) is still strictly before nbf.
[[nodiscard]] bool jwt_token_not_yet_valid(jwt_clock_parts now_parts,
    std::chrono::system_clock::time_point not_before, std::chrono::seconds leeway) noexcept {
    const auto not_before_parts = jwt_split_clock_time(not_before);
    const auto distance = not_before_parts.whole_seconds_ - now_parts.whole_seconds_;
    return distance > leeway ||
           (distance == leeway && not_before_parts.fraction_ > now_parts.fraction_);
}

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

[[nodiscard]] std::pmr::string jwt_base64_url_decode(std::string_view input, std::pmr::memory_resource* resource) {
    std::pmr::string out(resource);
    out.resize(http_base64_decoded_size_bound(input.size()));
    const auto decoded_size = http_base64_decode(input,
        std::span(reinterpret_cast<std::uint8_t*>(out.data()), out.size()),
        http_base64_charset::url_safe, http_base64_padding::forbidden);
    if (!decoded_size) {
        throw jwt_error(jwt_error::code_type::malformed_token, "JWT base64url value is invalid");
    }
    out.resize(*decoded_size);
    return out;
}

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

// Encodes the base64url signature into `out`. Throws std::invalid_argument for
// a key shorter than the selected digest and std::length_error when the secret
// or signing input exceeds OpenSSL's HMAC length parameters.
[[nodiscard]] std::string_view jwt_hmac_sign(jwt_algorithm algorithm, std::string_view secret,
    std::string_view data, std::array<char, 88>& out) {
    const auto digest = jwt_hmac_digest(algorithm);
    const auto digest_size = hmac_digest_size(digest);
    if (secret.size() > hmac_max_input_size) {
        throw std::length_error("JWT secret is too large");
    }
    // RFC 7518 section 3.2: the key must be at least as large as the hash output.
    if (secret.size() < digest_size) {
        throw std::invalid_argument("JWT secret is shorter than the selected algorithm's digest");
    }
    if (data.size() > hmac_max_input_size) {
        throw std::length_error("JWT signing input is too large");
    }
    std::array<std::uint8_t, hmac_max_digest_size> mac{};
    if (!compute_hmac(digest, secret, data, mac)) {
        throw std::runtime_error("JWT HMAC signing failed");
    }
    const auto encoded_size = http_base64_encoded_size(digest_size, http_base64_padding::forbidden);
    jwt_base64_url_encode_into(
        out.data(), std::string_view(reinterpret_cast<const char*>(mac.data()), digest_size));
    return std::string_view(out.data(), encoded_size);
}

}  // namespace

std::chrono::system_clock::time_point jwt_from_epoch_seconds(std::int64_t value) {
    // Use the same saturating conversion for integer NumericDates and offsets.
    // Converting unbounded seconds directly to clock ticks would overflow.
    return jwt_time_with_offset(std::chrono::system_clock::time_point{}, std::chrono::seconds{value});
}

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
        const auto requested = detail::jwt_time_with_offset(now, *options.expires_in_);
        const auto expires_seconds = options.expires_in_->count() == 0
                                         ? detail::jwt_epoch_seconds(requested)
                                         : std::chrono::ceil<std::chrono::seconds>(
                                               requested.time_since_epoch())
                                               .count();
        detail::jwt_append_json_member(payload, first, "exp", expires_seconds);
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
        detail::jwt_append_json_member(payload, first, claim.name_.view(), claim.value_.view());
    }
    payload.push_back('}');

    const auto encoded_header_size =
        http_base64_encoded_size(header.size(), http_base64_padding::forbidden);
    const auto encoded_payload_size =
        http_base64_encoded_size(payload.size(), http_base64_padding::forbidden);
    const auto signature_size = http_base64_encoded_size(
        detail::hmac_digest_size(detail::jwt_hmac_digest(options.algorithm_)),
        http_base64_padding::forbidden);
    if (encoded_header_size > std::numeric_limits<std::size_t>::max() - 1 ||
        encoded_payload_size > std::numeric_limits<std::size_t>::max() - 1 - (encoded_header_size + 1) ||
        signature_size > std::numeric_limits<std::size_t>::max() -
                             (encoded_header_size + 1 + encoded_payload_size + 1)) {
        throw std::length_error("JWT token is too large");
    }
    const auto signing_input_size = encoded_header_size + 1 + encoded_payload_size;
    std::pmr::string token(resolved);
    token.resize(signing_input_size + 1 + signature_size);
    detail::jwt_base64_url_encode_into(token.data(), header);
    token[encoded_header_size] = '.';
    detail::jwt_base64_url_encode_into(token.data() + encoded_header_size + 1, payload);
    std::array<char, 88> signature_buf{};
    const auto signature = detail::jwt_hmac_sign(
        options.algorithm_, options.secret_, std::string_view(token.data(), signing_input_size), signature_buf);
    token[signing_input_size] = '.';
    signature.copy(token.data() + signing_input_size + 1, signature.size());
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
    std::array<char, 88> expected_buf{};
    const auto expected =
        detail::jwt_hmac_sign(options.algorithm_, options.secret_, parts.signing_input_, expected_buf);
    if (!detail::constant_time_bytes_equal(expected, parts.signature_)) {
        throw jwt_error(jwt_error::code_type::signature_mismatch, "JWT signature verification failed");
    }
    const auto header_value = detail::jwt_base64_url_decode(parts.header_, resolved);
    if (detail::jwt_parse_jose_algorithm(header_value, resolved) !=
        detail::jwt_algorithm_name(options.algorithm_)) {
        throw jwt_error(jwt_error::code_type::algorithm_mismatch, "JWT algorithm mismatch");
    }
    const auto payload_json = detail::jwt_base64_url_decode(parts.payload_, resolved);
    auto payload_value = detail::jwt_payload_access::decode_payload_json(payload_json, resolved);
    if (options.expiration_claim_ == jwt_expiration_claim_policy::require && !payload_value.expires_at()) {
        throw jwt_error(jwt_error::code_type::missing_claim, "JWT token is missing exp claim");
    }
    if (payload_value.expires_at() || payload_value.not_before()) {
        const auto now_parts = detail::jwt_split_clock_time(std::chrono::system_clock::now());
        if (payload_value.expires_at() &&
            detail::jwt_token_expired(now_parts, *payload_value.expires_at(), options.leeway_)) {
            throw jwt_error(jwt_error::code_type::expired, "JWT token is expired");
        }
        if (payload_value.not_before() &&
            detail::jwt_token_not_yet_valid(now_parts, *payload_value.not_before(), options.leeway_)) {
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

jwt_payload jwt_decode_unverified(jwt_decode_unverified_options options) {
    auto* resolved = pmr_resource_or_default(options.resource_);
    const auto token = options.token_.view();
    const auto parts = detail::jwt_split_token(token);
    const auto header_value = detail::jwt_base64_url_decode(parts.header_, resolved);
    if (detail::jwt_parse_jose_algorithm(header_value, resolved).empty()) {
        throw jwt_error(jwt_error::code_type::invalid_header, "JWT JOSE header is invalid");
    }
    const auto payload_json = detail::jwt_base64_url_decode(parts.payload_, resolved);
    return detail::jwt_payload_access::decode_payload_json(payload_json, resolved);
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
