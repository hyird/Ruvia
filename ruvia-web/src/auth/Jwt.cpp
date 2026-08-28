#include "ruvia/web/auth/Jwt.h"

#include "ruvia/web/detail/auth/JwtPrimitives.h"

#include <algorithm>
#include <stdexcept>

namespace ruvia {
namespace {

void validateJwtCustomClaims(std::span<const JwtClaim> claims) {
    for (std::size_t i = 0; i < claims.size(); ++i) {
        const auto name = claims[i].name();
        if (name.empty() || detail::jwtIsReservedClaim(name)) {
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

JwtPayload::JwtPayload(JwtPayloadOptions options)
    : issuer_(detail::pmrResourceOrDefault(options.resource)),
      subject_(issuer_.get_allocator().resource()),
      audiences_(issuer_.get_allocator().resource()),
      id_(issuer_.get_allocator().resource()),
      claims_(issuer_.get_allocator().resource()) {}

std::string_view JwtPayload::issuer() const& noexcept {
    return issuer_;
}
std::string_view JwtPayload::subject() const& noexcept {
    return subject_;
}
std::string_view JwtPayload::audience() const& noexcept {
    return audiences_.empty() ? std::string_view{} : std::string_view(audiences_.front());
}
bool JwtPayload::hasAudience(std::string_view audience) const noexcept {
    return std::ranges::find(audiences_, audience, [](const auto& value) noexcept { return std::string_view(value); }) != audiences_.end();
}
std::string_view JwtPayload::id() const& noexcept {
    return id_;
}
std::optional<std::chrono::system_clock::time_point> JwtPayload::expiresAt() const noexcept {
    return expiresAt_;
}
std::optional<std::chrono::system_clock::time_point> JwtPayload::notBefore() const noexcept {
    return notBefore_;
}
std::optional<std::chrono::system_clock::time_point> JwtPayload::issuedAt() const noexcept {
    return issuedAt_;
}
std::span<const JwtClaim> JwtPayload::claims() const& noexcept {
    return claims_;
}

std::optional<std::string_view> JwtPayload::claim(std::string_view name) const& noexcept {
    for (const auto& item : claims_) {
        if (item.name() == name) {
            return item.value();
        }
    }
    return std::nullopt;
}

std::pmr::string jwtSign(const JwtSignOptions& options) {
    validateJwtCustomClaims(options.claims);
    if ((options.expiresIn.has_value() && options.expiresIn->count() < 0) || (options.notBeforeDelay.has_value() && options.notBeforeDelay->count() < 0)) {
        throw std::invalid_argument("JWT signing time offsets must not be negative");
    }
    auto* resolved = detail::pmrResourceOrDefault(options.resource);
    const auto now = std::chrono::system_clock::now();
    std::pmr::string header(resolved);
    header.append("{\"alg\":");
    detail::jwtAppendJsonEscaped(header, detail::jwtAlgorithmName(options.algorithm));
    header.append(",\"typ\":\"JWT\"}");

    std::pmr::string payload(resolved);
    payload.push_back('{');
    bool first = true;
    if (!options.issuer.empty()) {
        detail::jwtAppendJsonMember(payload, first, "iss", options.issuer);
    }
    if (!options.subject.empty()) {
        detail::jwtAppendJsonMember(payload, first, "sub", options.subject);
    }
    if (!options.audience.empty()) {
        detail::jwtAppendJsonMember(payload, first, "aud", options.audience);
    }
    if (!options.id.empty()) {
        detail::jwtAppendJsonMember(payload, first, "jti", options.id);
    }
    detail::jwtAppendJsonMember(payload, first, "iat", detail::jwtEpochSeconds(now));
    if (options.expiresIn.has_value()) {
        detail::jwtAppendJsonMember(payload, first, "exp", detail::jwtEpochSeconds(detail::jwtTimeWithOffset(now, *options.expiresIn)));
    }
    if (options.notBeforeDelay.has_value()) {
        detail::jwtAppendJsonMember(payload, first, "nbf", detail::jwtEpochSeconds(detail::jwtTimeWithOffset(now, *options.notBeforeDelay)));
    }
    for (const auto& claim : options.claims) {
        detail::jwtAppendJsonMember(payload, first, claim.name(), claim.value());
    }
    payload.push_back('}');

    auto encodedHeader = detail::jwtBase64UrlEncode(header, resolved);
    auto encodedPayload = detail::jwtBase64UrlEncode(payload, resolved);
    std::pmr::string signingInput(resolved);
    signingInput.append(encodedHeader);
    signingInput.push_back('.');
    signingInput.append(encodedPayload);
    auto signature = detail::jwtHmacSign(options.algorithm, options.secret, signingInput, resolved);
    signingInput.push_back('.');
    signingInput.append(signature);
    return signingInput;
}

JwtPayload jwtVerify(const JwtVerifyOptions& options) {
    if (options.leeway.count() < 0) {
        throw std::invalid_argument("JWT verification leeway must not be negative");
    }
    if (options.expirationClaim != JwtExpirationClaimPolicy::kRequire && options.expirationClaim != JwtExpirationClaimPolicy::kAllowMissing) {
        throw std::invalid_argument("JWT expiration claim policy is invalid");
    }
    auto* resolved = detail::pmrResourceOrDefault(options.resource);
    const auto token = options.token.view();
    const auto parts = detail::jwtSplitToken(token);
    const auto expected = detail::jwtHmacSign(options.algorithm, options.secret, parts.signingInput, resolved);
    if (!detail::jwtConstantTimeEquals(expected, parts.signature)) {
        throw std::runtime_error("JWT signature verification failed");
    }
    const auto header = detail::jwtBase64UrlDecode(parts.header, resolved);
    if (detail::jwtParseJoseAlgorithm(header, resolved) != detail::jwtAlgorithmName(options.algorithm)) {
        throw std::runtime_error("JWT algorithm mismatch");
    }
    const auto payloadJson = detail::jwtBase64UrlDecode(parts.payload, resolved);
    auto payload = detail::JwtPayloadAccess::decodePayloadJson(payloadJson, resolved);
    const auto now = std::chrono::system_clock::now();
    if (options.expirationClaim == JwtExpirationClaimPolicy::kRequire && !payload.expiresAt()) {
        throw std::runtime_error("JWT token is missing exp claim");
    }
    if (payload.expiresAt() && detail::jwtTokenExpired(now, *payload.expiresAt(), options.leeway)) {
        throw std::runtime_error("JWT token is expired");
    }
    if (payload.notBefore() && detail::jwtTokenNotYetValid(now, *payload.notBefore(), options.leeway)) {
        throw std::runtime_error("JWT token is not yet valid");
    }
    if (!options.issuer.empty() && payload.issuer() != options.issuer) {
        throw std::runtime_error("JWT issuer mismatch");
    }
    if (!options.subject.empty() && payload.subject() != options.subject) {
        throw std::runtime_error("JWT subject mismatch");
    }
    if (!options.audience.empty() && !payload.hasAudience(options.audience)) {
        throw std::runtime_error("JWT audience mismatch");
    }
    return payload;
}

JwtPayload jwtDecodeUnverified(JwtDecodeUnverifiedOptions options) {
    auto* resolved = detail::pmrResourceOrDefault(options.resource);
    const auto token = options.token.view();
    const auto parts = detail::jwtSplitToken(token);
    const auto payloadJson = detail::jwtBase64UrlDecode(parts.payload, resolved);
    return detail::JwtPayloadAccess::decodePayloadJson(payloadJson, resolved);
}

std::optional<std::string_view> jwtBearerToken(std::string_view authorization) noexcept {
    constexpr std::string_view scheme = "Bearer";
    if (authorization.size() <= scheme.size()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < scheme.size(); ++i) {
        const auto left = authorization[i] >= 'A' && authorization[i] <= 'Z' ? authorization[i] + 32 : authorization[i];
        const auto right = scheme[i] >= 'A' && scheme[i] <= 'Z' ? scheme[i] + 32 : scheme[i];
        if (left != right) {
            return std::nullopt;
        }
    }
    if (authorization[scheme.size()] != ' ') {
        return std::nullopt;
    }
    auto tokenOffset = scheme.size();
    while (tokenOffset < authorization.size() && authorization[tokenOffset] == ' ') {
        ++tokenOffset;
    }
    if (tokenOffset == authorization.size()) {
        return std::nullopt;
    }
    return authorization.substr(tokenOffset);
}

}  // namespace ruvia
